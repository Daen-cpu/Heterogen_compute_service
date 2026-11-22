#include "hetero/simulation_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <iomanip>

namespace hetero
{

SimulationBackend::SimulationBackend()
{
    configure(config_);
    thread_ = std::thread(&SimulationBackend::run, this);
}

SimulationBackend::~SimulationBackend()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }

    cv_.notify_all();

    if (thread_.joinable())
    {
        thread_.join();
    }
}

void SimulationBackend::initialize_workers(std::uint32_t count)
{
    static constexpr double factors[] = {0.92, 1.0, 1.07};

    workers_.assign(count, Worker{});

    for (std::size_t i = 0; i < workers_.size(); ++i)
    {
        workers_[i].speed = factors[i];
        workers_[i].state = WorkerState::Idle;
        workers_[i].enabled = true;
        workers_[i].ready_asserted = false;
    }
}

void SimulationBackend::configure(const Config& c)
{
    std::lock_guard lock(mutex_);

    if (running_)
    {
        throw std::logic_error("cannot configure a running job");
    }

    if (c.matrix_size == 0 || c.tile_size == 0 || c.matrix_size % c.tile_size != 0 ||
        c.worker_count == 0 || c.worker_count > 3 || c.spi_mbps <= 0 || c.wall_delay_ms < 0)
    {
        throw std::invalid_argument("invalid simulation configuration");
    }

    config_ = c;
    initialize_workers(c.worker_count);
    initialize_matrices();

    tasks_.clear();
    events_ = {};
    history_.clear();

    sim_time_us_ = 0;
    spi_free_at_ = 0;
    sequence_ = 0;
    rr_worker_cursor_ = 0;
    paused_ = false;
    computed_tiles_ = 0;
    relative_error_ = 0.0;
    numerical_validation_done_ = false;
}

void SimulationBackend::start()
{
    std::lock_guard lock(mutex_);

    if (running_)
    {
        return;
    }

    if (tasks_.empty())
    {
        const auto n = config_.matrix_size / config_.tile_size;
        std::uint32_t id = 0;

        for (std::uint32_t r = 0; r < n; ++r)
        {
            for (std::uint32_t c = 0; c < n; ++c)
            {
                tasks_.push_back(Task{id++, r, c, "queued", -1});

                history_.push_back({sim_time_us_, "TASK_CREATED", id - 1, 0});
            }
        }
    }

    running_ = true;
    paused_ = false;

    dispatch();
    cv_.notify_all();
}

void SimulationBackend::pause()
{
    std::lock_guard lock(mutex_);
    paused_ = true;
}

void SimulationBackend::resume()
{
    std::lock_guard lock(mutex_);

    if (running_)
    {
        paused_ = false;
    }

    cv_.notify_all();
}

void SimulationBackend::reset()
{
    std::lock_guard lock(mutex_);

    running_ = false;
    paused_ = false;

    events_ = {};
    tasks_.clear();
    history_.clear();

    sim_time_us_ = 0;
    spi_free_at_ = 0;
    sequence_ = 0;
    rr_worker_cursor_ = 0;

    initialize_workers(config_.worker_count);
    initialize_matrices();
    computed_tiles_ = 0;
    relative_error_ = 0.0;
    numerical_validation_done_ = false;
}

void SimulationBackend::set_policy(Policy p)
{
    std::lock_guard lock(mutex_);

    policy_ = p;
    dispatch();
}

void SimulationBackend::set_worker_enabled(std::uint32_t id, bool enabled)
{
    std::lock_guard lock(mutex_);

    if (id >= workers_.size())
    {
        throw std::out_of_range("worker id");
    }

    auto& w = workers_[id];

    if (!enabled && w.state == WorkerState::Idle)
    {
        w.enabled = false;
        w.state = WorkerState::Failed;
    }
    else if (enabled && w.state == WorkerState::Failed)
    {
        w.enabled = true;
        w.state = WorkerState::Idle;
        dispatch();
    }
}

void SimulationBackend::schedule(EventType t, double at, std::uint32_t w, std::uint32_t task)
{
    events_.push(Event{at, sequence_++, t, w, task});
}

void SimulationBackend::initialize_matrices()
{
    const std::size_t n = config_.matrix_size;
    const std::size_t elements = n * n;
    matrix_a_.resize(elements);
    matrix_b_.resize(elements);
    matrix_c_.assign(elements, 0.0F);
    reference_c_.assign(elements, 0.0F);

    for (std::size_t row = 0; row < n; ++row)
    {
        for (std::size_t col = 0; col < n; ++col)
        {
            const std::size_t index = row * n + col;
            const int a = static_cast<int>((row * 17U + col * 13U + 7U) % 23U) - 11;
            const int b = static_cast<int>((row * 7U + col * 19U + 3U) % 29U) - 14;
            matrix_a_[index] = static_cast<float>(a) / 16.0F;
            matrix_b_[index] = static_cast<float>(b) / 16.0F;
        }
    }

    for (std::size_t row = 0; row < n; ++row)
    {
        for (std::size_t col = 0; col < n; ++col)
        {
            double sum = 0.0;
            for (std::size_t k = 0; k < n; ++k)
                sum += static_cast<double>(matrix_a_[row * n + k]) *
                       static_cast<double>(matrix_b_[k * n + col]);
            reference_c_[row * n + col] = static_cast<float>(sum);
        }
    }
}

void SimulationBackend::compute_output_tile(const Task& task)
{
    const std::size_t n = config_.matrix_size;
    const std::size_t block = config_.tile_size;
    const std::size_t row_begin = static_cast<std::size_t>(task.row) * block;
    const std::size_t col_begin = static_cast<std::size_t>(task.col) * block;

    for (std::size_t row = row_begin; row < row_begin + block; ++row)
    {
        for (std::size_t col = col_begin; col < col_begin + block; ++col)
        {
            double sum = 0.0;
            for (std::size_t k = 0; k < n; ++k)
                sum += static_cast<double>(matrix_a_[row * n + k]) *
                       static_cast<double>(matrix_b_[k * n + col]);
            matrix_c_[row * n + col] = static_cast<float>(sum);
        }
    }
    ++computed_tiles_;
}

double SimulationBackend::calculate_relative_error() const
{
    double diff_sq = 0.0;
    double ref_sq = 0.0;
    for (std::size_t i = 0; i < matrix_c_.size(); ++i)
    {
        const double diff = static_cast<double>(matrix_c_[i]) - static_cast<double>(reference_c_[i]);
        const double ref = static_cast<double>(reference_c_[i]);
        diff_sq += diff * diff;
        ref_sq += ref * ref;
    }
    if (ref_sq == 0.0)
        return diff_sq == 0.0 ? 0.0 : std::numeric_limits<double>::infinity();
    return std::sqrt(diff_sq / ref_sq);
}

double SimulationBackend::input_transfer_us() const
{
    const double b = config_.tile_size;
    const double bytes = 2.0 * config_.matrix_size * b * sizeof(float);

    return bytes * 8.0 / (config_.spi_mbps * 1e6) * 1e6 + 35.0;
}

double SimulationBackend::result_transfer_us() const
{
    const double b = config_.tile_size;
    const double bytes = b * b * sizeof(float);

    return bytes * 8.0 / (config_.spi_mbps * 1e6) * 1e6 + 20.0;
}

double SimulationBackend::compute_us(const Worker& w) const
{
    const double b = config_.tile_size;
    const double ops = 2.0 * b * b * config_.matrix_size;
    const double base_mops = 35.0 * w.speed;

    return ops / (base_mops * 1e6) * 1e6 + 80.0;
}

int SimulationBackend::choose_task(std::uint32_t worker) const
{
    (void)worker;

    for (std::size_t i = 0; i < tasks_.size(); ++i)
    {
        if (tasks_[i].state == "queued")
        {
            return static_cast<int>(i);
        }
    }

    return -1;
}

double SimulationBackend::estimate_completion_time(
    std::uint32_t worker_id,
    std::uint32_t task_index) const
{
    if (worker_id >= workers_.size() || task_index >= tasks_.size())
    {
        return std::numeric_limits<double>::infinity();
    }

    const auto& candidate_worker = workers_[worker_id];

    // A new input transfer is appended after all SPI transfers already reserved.
    const double candidate_input_begin = std::max(sim_time_us_, spi_free_at_);
    const double candidate_input_done =
        candidate_input_begin + input_transfer_us();

    const double candidate_compute_done =
        candidate_input_done + compute_us(candidate_worker);

    struct PendingOutput
    {
        double ready_at{};
        std::uint32_t worker_id{};
        std::uint32_t task_id{};
        bool candidate{};
    };

    std::vector<PendingOutput> pending_outputs;
    pending_outputs.reserve(workers_.size() + 1);

    // Predict the output-ready time of each currently active worker.
    // Transfers already in ResultTransfer state have already reserved their
    // SPI interval and are therefore included in spi_free_at_.
    for (std::uint32_t i = 0; i < workers_.size(); ++i)
    {
        const auto& worker = workers_[i];

        if (worker.task < 0 || i == worker_id)
        {
            continue;
        }

        const auto task_index_active = static_cast<std::size_t>(worker.task);
        if (task_index_active >= tasks_.size())
        {
            continue;
        }

        const auto task_id_active = tasks_[task_index_active].id;

        if (worker.state == WorkerState::Idle ||
            worker.state == WorkerState::Failed ||
            (worker.state == WorkerState::Transfer && !worker.input_transfer_active))
        {
            continue;
        }

        std::optional<double> event_time;
        auto queued_events = events_;

        while (!queued_events.empty())
        {
            const auto event = queued_events.top();
            queued_events.pop();

            if (event.worker != i || event.task != task_id_active)
            {
                continue;
            }

            if ((worker.state == WorkerState::Transfer &&
                 worker.input_transfer_active &&
                 event.type == EventType::InputDone) ||
                (worker.state == WorkerState::Busy &&
                 event.type == EventType::ComputeDone) ||
                (worker.state == WorkerState::WaitResult &&
                 event.type == EventType::ReadyAsserted))
            {
                event_time = event.at;
                break;
            }
        }

        if (!event_time)
        {
            continue;
        }

        double output_ready_at = *event_time;

        if (worker.state == WorkerState::Transfer && worker.input_transfer_active)
        {
            output_ready_at += compute_us(worker);
        }

        pending_outputs.push_back({
            output_ready_at,
            i,
            task_id_active,
            false
        });
    }

    // Include the candidate task in the predicted output queue.
    pending_outputs.push_back({
        candidate_compute_done,
        worker_id,
        tasks_[task_index].id,
        true
    });

    std::stable_sort(
        pending_outputs.begin(),
        pending_outputs.end(),
        [](const PendingOutput& lhs, const PendingOutput& rhs)
        {
            if (lhs.ready_at != rhs.ready_at)
            {
                return lhs.ready_at < rhs.ready_at;
            }

            if (lhs.worker_id != rhs.worker_id)
            {
                return lhs.worker_id < rhs.worker_id;
            }

            return lhs.task_id < rhs.task_id;
        });

    // All already-reserved SPI intervals, including the candidate input,
    // precede these not-yet-reserved result transfers.
    double projected_spi_free_at = candidate_input_done;

    for (const auto& output : pending_outputs)
    {
        const double transfer_begin =
            std::max(output.ready_at, projected_spi_free_at);

        projected_spi_free_at =
            transfer_begin + result_transfer_us();

        if (output.candidate)
        {
            return projected_spi_free_at - sim_time_us_;
        }
    }

    return std::numeric_limits<double>::infinity();
}

std::uint32_t SimulationBackend::choose_worker() const
{
    std::vector<std::uint32_t> idle_workers;

    for (std::uint32_t i = 0; i < workers_.size(); ++i)
    {
        if (workers_[i].enabled && workers_[i].state == WorkerState::Idle)
        {
            idle_workers.push_back(i);
        }
    }

    if (idle_workers.empty())
    {
        return static_cast<std::uint32_t>(workers_.size());
    }

    if (policy_ == Policy::RoundRobin)
    {
        for (std::size_t offset = 0; offset < workers_.size(); ++offset)
        {
            const auto candidate =
                (rr_worker_cursor_ + static_cast<std::uint32_t>(offset)) %
                static_cast<std::uint32_t>(workers_.size());

            if (workers_[candidate].enabled &&
                workers_[candidate].state == WorkerState::Idle)
            {
                return candidate;
            }
        }
    }

    const int first_task = choose_task(0);
    if (first_task < 0)
    {
        return static_cast<std::uint32_t>(workers_.size());
    }

    auto best_worker = idle_workers.front();
    double best_score = std::numeric_limits<double>::infinity();

    for (const auto candidate : idle_workers)
    {
        const double predicted_finish =
            estimate_completion_time(candidate, static_cast<std::uint32_t>(first_task));

        // LeastLoaded balances accumulated service demand, while including
        // the candidate's predicted compute and shared-SPI completion time.
        // Predicted minimizes only the projected completion time of this assignment.
        const double score =
            policy_ == Policy::LeastLoaded
                ? workers_[candidate].busy_us + predicted_finish
                : predicted_finish;

        if (score < best_score ||
            (score == best_score && candidate < best_worker))
        {
            best_score = score;
            best_worker = candidate;
        }
    }

    return best_worker;
}

void SimulationBackend::dispatch()
{
    if (!running_ || paused_)
    {
        return;
    }

    while (true)
    {
        const int task_index = choose_task(0);
        if (task_index < 0)
        {
            return;
        }

        const auto worker_id = choose_worker();
        if (worker_id >= workers_.size())
        {
            return;
        }

        auto& worker = workers_[worker_id];
        auto& task = tasks_[static_cast<std::size_t>(task_index)];

        task.state = "dispatched";
        task.worker = static_cast<int>(worker_id);

        history_.push_back({
            sim_time_us_,
            "TASK_DISPATCHED",
            task.id,
            worker_id
        });

        worker.task = task_index;
        worker.state = WorkerState::Transfer;
        worker.state_since = sim_time_us_;
        worker.input_transfer_active = true;

        const double input_begin = std::max(sim_time_us_, spi_free_at_);
        const double input_done = input_begin + input_transfer_us();

        spi_free_at_ = input_done;

        schedule(EventType::InputStart, input_begin, worker_id, task.id);
        schedule(EventType::InputDone, input_done, worker_id, task.id);

        if (policy_ == Policy::RoundRobin)
        {
            rr_worker_cursor_ =
                (worker_id + 1) % static_cast<std::uint32_t>(workers_.size());
        }
    }
}

void SimulationBackend::process_one()
{
    if (events_.empty())
    {
        running_ = false;
        return;
    }

    auto e = events_.top();
    events_.pop();

    sim_time_us_ = e.at;

    if (e.worker >= workers_.size() || e.task >= tasks_.size())
    {
        return;
    }

    auto& w = workers_[e.worker];
    auto& t = tasks_[e.task];

    switch (e.type)
    {
    case EventType::InputStart:
        history_.push_back({sim_time_us_, "SPI_INPUT_TRANSFER_STARTED", e.task, e.worker});
        break;

    case EventType::InputDone:
        w.input_transfer_active = false;
        w.state = WorkerState::Busy;
        w.state_since = sim_time_us_;
        t.state = "computing";

        history_.push_back({sim_time_us_, "SPI_INPUT_TRANSFER_COMPLETED", e.task, e.worker});

        history_.push_back({sim_time_us_, "COMPUTATION_STARTED", e.task, e.worker});

        schedule(EventType::ComputeDone, sim_time_us_ + compute_us(w), e.worker, e.task);
        break;

    case EventType::ComputeDone:
        compute_output_tile(t);
        w.busy_us += sim_time_us_ - w.state_since;
        w.state = WorkerState::WaitResult;
        t.state = "ready";

        history_.push_back({sim_time_us_, "COMPUTATION_COMPLETED", e.task, e.worker});

        schedule(EventType::ReadyAsserted, sim_time_us_, e.worker, e.task);
        break;

    case EventType::ReadyAsserted:
    {
        history_.push_back({sim_time_us_, "READY_ASSERTED", e.task, e.worker});

        w.ready_asserted = true;

        const double begin = std::max(sim_time_us_, spi_free_at_);
        spi_free_at_ = begin + result_transfer_us();

        w.state = WorkerState::WaitResult;

        schedule(EventType::ResultStart, begin, e.worker, e.task);
        schedule(EventType::ResultDone, spi_free_at_, e.worker, e.task);

        break;
    }

    case EventType::ResultStart:
        w.input_transfer_active = false;
        w.state = WorkerState::Transfer;
        w.state_since = sim_time_us_;
        history_.push_back({sim_time_us_, "SPI_RESULT_TRANSFER_STARTED", e.task, e.worker});
        break;

    case EventType::ResultDone:
        history_.push_back({sim_time_us_, "RESULT_VALIDATED", e.task, e.worker});

        schedule(EventType::AckSent, sim_time_us_, e.worker, e.task);
        break;

    case EventType::AckSent:
        history_.push_back({sim_time_us_, "ACK_SENT", e.task, e.worker});

        w.ready_asserted = false;
        history_.push_back({sim_time_us_, "READY_DEASSERTED", e.task, e.worker});
        schedule(EventType::WorkerIdle, sim_time_us_, e.worker, e.task);
        break;

    case EventType::WorkerIdle:
        t.state = "completed";
        w.completed++;
        w.task = -1;
        w.input_transfer_active = false;
        w.state = w.enabled ? WorkerState::Idle : WorkerState::Failed;

        history_.push_back({sim_time_us_, "WORKER_IDLE", e.task, e.worker});

        const bool all_completed = std::all_of(
            tasks_.begin(), tasks_.end(),
            [](const Task& task) { return task.state == "completed"; });

        if (all_completed && !numerical_validation_done_)
        {
            relative_error_ = calculate_relative_error();
            numerical_validation_done_ = true;
            history_.push_back({
                sim_time_us_,
                relative_error_ <= 1e-6 ? "MATRIX_VALIDATION_PASSED" : "MATRIX_VALIDATION_FAILED",
                e.task,
                e.worker
            });

            double total_busy_us = 0.0;
            nlohmann::json worker_results = nlohmann::json::array();
            for (std::size_t worker_index = 0; worker_index < workers_.size(); ++worker_index)
            {
                const auto& worker = workers_[worker_index];
                total_busy_us += worker.busy_us;
                worker_results.push_back({
                    {"worker_id", worker_index},
                    {"completed_tiles", worker.completed},
                    {"busy_us", worker.busy_us}
                });
            }
            const double run_utilization = sim_time_us_ > 0.0
                ? total_busy_us / (sim_time_us_ * static_cast<double>(workers_.size()))
                : 0.0;
            run_history_.push_back({
                {"run_id", ++run_sequence_},
                {"policy", to_string(policy_)},
                {"matrix_size", config_.matrix_size},
                {"tile_size", config_.tile_size},
                {"worker_count", workers_.size()},
                {"total_tasks", tasks_.size()},
                {"sim_time_us", sim_time_us_},
                {"worker_utilization", run_utilization},
                {"relative_frobenius_error", relative_error_},
                {"passed", relative_error_ <= 1e-6},
                {"workers", std::move(worker_results)}
            });
            if (run_history_.size() > 3)
                run_history_.erase(run_history_.begin());
        }

        dispatch();
        break;
    }
}

void SimulationBackend::run()
{
    std::unique_lock lock(mutex_);

    while (!stopping_)
    {
        cv_.wait(lock, [&] { return stopping_ || (running_ && !paused_ && !events_.empty()); });

        if (stopping_)
        {
            break;
        }

        const auto delay = std::chrono::duration<double, std::milli>(config_.wall_delay_ms);

        lock.unlock();
        std::this_thread::sleep_for(delay);
        lock.lock();

        if (running_ && !paused_)
        {
            process_one();
        }

        if (running_ && events_.empty())
        {
            const bool unfinished = std::any_of(
                tasks_.begin(), tasks_.end(), [](const Task& t) { return t.state != "completed"; });

            if (!unfinished)
            {
                running_ = false;
            }
            else
            {
                dispatch();
            }
        }
    }
}

nlohmann::json SimulationBackend::snapshot() const
{
    std::lock_guard lock(mutex_);

    double total_busy = 0;

    for (const auto& w : workers_)
    {
        total_busy += w.busy_us;
    }

    const double utilization =
        sim_time_us_ > 0 ? total_busy / (sim_time_us_ * std::max<std::size_t>(1, workers_.size()))
                         : 0;

    nlohmann::json ws = nlohmann::json::array();
    nlohmann::json ts = nlohmann::json::array();
    nlohmann::json ev = nlohmann::json::array();

    for (std::size_t i = 0; i < workers_.size(); ++i)
    {
        auto& w = workers_[i];

        ws.push_back({{"id", i},
                      {"state", to_string(w.state)},
                      {"speed_factor", w.speed},
                      {"completed_tiles", w.completed},
                      {"busy_us", w.busy_us},
                      {"ready_asserted", w.ready_asserted},
                      {"ready_line", w.ready_asserted ? "LOW" : "HIGH"},
                      {"transfer_phase",
                       w.state == WorkerState::Transfer
                           ? (w.input_transfer_active ? "INPUT" : "RESULT")
                           : "NONE"},
                      {"task_id",
                       w.task < 0 ? nlohmann::json(nullptr)
                                  : nlohmann::json(tasks_[static_cast<std::size_t>(w.task)].id)}});
    }

    for (const auto& t : tasks_)
    {
        ts.push_back(
            {{"id", t.id},
             {"row", t.row},
             {"col", t.col},
             {"state", t.state},
             {"worker_id", t.worker < 0 ? nlohmann::json(nullptr) : nlohmann::json(t.worker)}});
    }

    const std::size_t from = history_.size() > 100 ? history_.size() - 100 : 0;

    for (std::size_t i = from; i < history_.size(); ++i)
    {
        auto& e = history_[i];

        ev.push_back({{"time_us", e.time_us},
                      {"type", e.type},
                      {"task_id", e.task_id},
                      {"worker_id", e.worker_id}});
    }

    const std::size_t done = std::count_if(tasks_.begin(), tasks_.end(),
                                           [](const auto& t) { return t.state == "completed"; });

    return {{"backend_mode", "simulation"},
            {"job_state",
             to_string(!running_ && !tasks_.empty() && done == tasks_.size() ? JobState::Completed
                       : running_ ? (paused_ ? JobState::Paused : JobState::Running)
                                  : JobState::Idle)},
            {"policy", to_string(policy_)},
            {"sim_time_us", sim_time_us_},
            {"spi_busy_until_us", spi_free_at_},
            {"matrix_size", config_.matrix_size},
            {"tile_size", config_.tile_size},
            {"worker_count", workers_.size()},
            {"total_tasks", tasks_.size()},
            {"completed_tasks", done},
            {"progress",
             tasks_.empty() ? 0.0 : static_cast<double>(done) / static_cast<double>(tasks_.size())},
            {"worker_utilization", utilization},
            {"numerical_validation",
             {{"done", numerical_validation_done_},
              {"computed_tiles", computed_tiles_},
              {"total_tiles", tasks_.size()},
              {"relative_frobenius_error", numerical_validation_done_ ? nlohmann::json(relative_error_) : nlohmann::json(nullptr)},
              {"passed", numerical_validation_done_ && relative_error_ <= 1e-6}}},
            {"workers", ws},
            {"tasks", ts},
            {"events", ev},
            {"run_history", run_history_}};
}

std::string SimulationBackend::export_log() const
{
    std::lock_guard lock(mutex_);
    std::ostringstream output;
    output << "ЖУРНАЛ СОБЫТИЙ ВЫЧИСЛИТЕЛЬНОГО ЭКСПЕРИМЕНТА\n";
    output << "Размер матрицы: " << config_.matrix_size << " x " << config_.matrix_size << "\n";
    output << "Размер блока: " << config_.tile_size << " x " << config_.tile_size << "\n";
    output << "Количество исполнителей: " << config_.worker_count << "\n";
    output << "Политика диспетчеризации: " << to_string(policy_) << "\n";
    output << "Виртуальное время: " << std::fixed << std::setprecision(2) << sim_time_us_ << " мкс\n";
    output << "Событий: " << history_.size() << "\n\n";
    output << "Время, мкс\tСобытие\tЗадача\tИсполнитель\n";
    const auto event_name = [](const std::string& type) -> const char*
    {
        if (type == "TASK_CREATED") return "ЗАДАЧА СОЗДАНА";
        if (type == "SPI_INPUT_TRANSFER_STARTED") return "НАЧАЛО ПЕРЕДАЧИ ВХОДНЫХ ДАННЫХ";
        if (type == "SPI_INPUT_TRANSFER_COMPLETED") return "ПЕРЕДАЧА ВХОДНЫХ ДАННЫХ ЗАВЕРШЕНА";
        if (type == "COMPUTATION_STARTED") return "ВЫЧИСЛЕНИЕ НАЧАТО";
        if (type == "COMPUTATION_COMPLETED") return "ВЫЧИСЛЕНИЕ ЗАВЕРШЕНО";
        if (type == "READY_ASSERTED") return "СИГНАЛ READY АКТИВЕН";
        if (type == "SPI_RESULT_TRANSFER_STARTED") return "НАЧАЛО ПЕРЕДАЧИ РЕЗУЛЬТАТА";
        if (type == "RESULT_VALIDATED") return "ПОЛУЧЕНИЕ РЕЗУЛЬТАТА ЗАВЕРШЕНО";
        if (type == "ACK_SENT") return "ПОДТВЕРЖДЕНИЕ ACK ОТПРАВЛЕНО";
        if (type == "READY_DEASSERTED") return "СИГНАЛ READY СБРОШЕН";
        if (type == "WORKER_IDLE") return "ИСПОЛНИТЕЛЬ СВОБОДЕН";
        if (type == "MATRIX_VALIDATION_PASSED") return "ПРОВЕРКА МАТРИЦЫ ПРОЙДЕНА";
        if (type == "MATRIX_VALIDATION_FAILED") return "ОШИБКА ПРОВЕРКИ МАТРИЦЫ";
        return type.c_str();
    };
    for (const auto& event : history_)
    {
        output << std::fixed << std::setprecision(2) << event.time_us << '\t'
               << event_name(event.type) << '\t' << event.task_id << '\t'
               << (event.worker_id + 1) << '\n';
    }
    if (numerical_validation_done_)
    {
        output << "\nОтносительная ошибка Фробениуса: " << std::scientific
               << relative_error_ << '\n';
        output << "Проверка: " << (relative_error_ <= 1e-6 ? "ПРОЙДЕНА" : "НЕ ПРОЙДЕНА") << '\n';
    }
    return output.str();
}

} // namespace hetero