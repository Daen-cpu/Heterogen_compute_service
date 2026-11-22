#include "hetero/simulation_backend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

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
    workers_.assign(c.worker_count, Worker{});

    const double factors[3] = {0.92, 1.0, 1.07};

    for (std::size_t i = 0; i < workers_.size(); ++i)
    {
        workers_[i].speed = factors[i];
    }

    tasks_.clear();
    events_ = {};
    history_.clear();

    sim_time_us_ = 0;
    spi_free_at_ = 0;
    sequence_ = 0;
    rr_cursor_ = 0;
    paused_ = false;
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
    rr_cursor_ = 0;

    workers_.assign(config_.worker_count, Worker{});

    const double factors[3] = {0.92, 1.0, 1.07};

    for (std::size_t i = 0; i < workers_.size(); ++i)
    {
        workers_[i].speed = factors[i];
    }
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
        w.state = WorkerState::Disabled;
    }
    else if (enabled && w.state == WorkerState::Disabled)
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

int SimulationBackend::choose_task(std::uint32_t worker)
{
    std::vector<int> q;

    for (std::size_t i = 0; i < tasks_.size(); ++i)
    {
        if (tasks_[i].state == "queued")
        {
            q.push_back(static_cast<int>(i));
        }
    }

    if (q.empty())
    {
        return -1;
    }

    if (policy_ == Policy::RoundRobin)
    {
        auto it = std::find_if(q.begin(), q.end(),
                               [&](int x) { return static_cast<std::uint32_t>(x) >= rr_cursor_; });

        const int chosen = it == q.end() ? q.front() : *it;

        rr_cursor_ = static_cast<std::uint32_t>(chosen + 1);

        return chosen;
    }

    if (policy_ == Policy::LeastLoaded)
    {
        return *std::min_element(q.begin(), q.end(),
                                 [&](int a, int b) { return tasks_[a].id < tasks_[b].id; });
    }

    // Predicted: assign the next task to the fastest currently idle executor.
    // Queue order remains deterministic.
    (void)worker;

    return q.front();
}

void SimulationBackend::dispatch()
{
    if (!running_ || paused_)
    {
        return;
    }

    std::vector<std::uint32_t> order;

    for (std::uint32_t i = 0; i < workers_.size(); ++i)
    {
        order.push_back(i);
    }

    if (policy_ == Policy::Predicted)
    {
        std::stable_sort(order.begin(), order.end(),
                         [&](auto a, auto b) { return workers_[a].speed > workers_[b].speed; });
    }
    else if (policy_ == Policy::LeastLoaded)
    {
        std::stable_sort(order.begin(), order.end(),
                         [&](auto a, auto b)
                         { return workers_[a].completed < workers_[b].completed; });
    }
    else if (!order.empty())
    {
        std::rotate(order.begin(), order.begin() + (rr_cursor_ % order.size()), order.end());
    }

    for (std::uint32_t wi : order)
    {
        auto& w = workers_[wi];

        if (!w.enabled || w.state != WorkerState::Idle)
        {
            continue;
        }

        const int ti = choose_task(wi);

        if (ti < 0)
        {
            break;
        }

        auto& t = tasks_[static_cast<std::size_t>(ti)];

        t.state = "dispatched";
        t.worker = static_cast<int>(wi);

        history_.push_back({sim_time_us_, "TASK_DISPATCHED", t.id, wi});

        w.task = ti;
        w.state = WorkerState::InputTransfer;
        w.state_since = sim_time_us_;

        const double begin = std::max(sim_time_us_, spi_free_at_);
        const double duration = input_transfer_us();

        spi_free_at_ = begin + duration;

        schedule(EventType::InputStart, begin, wi, t.id);
        schedule(EventType::InputDone, spi_free_at_, wi, t.id);
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
        w.state = WorkerState::Computing;
        w.state_since = sim_time_us_;
        t.state = "computing";

        history_.push_back({sim_time_us_, "SPI_INPUT_TRANSFER_COMPLETED", e.task, e.worker});

        history_.push_back({sim_time_us_, "COMPUTATION_STARTED", e.task, e.worker});

        schedule(EventType::ComputeDone, sim_time_us_ + compute_us(w), e.worker, e.task);
        break;

    case EventType::ComputeDone:
        w.busy_us += sim_time_us_ - w.state_since;
        w.state = WorkerState::Ready;
        t.state = "ready";

        history_.push_back({sim_time_us_, "COMPUTATION_COMPLETED", e.task, e.worker});

        schedule(EventType::ReadyAsserted, sim_time_us_, e.worker, e.task);
        break;

    case EventType::ReadyAsserted:
    {
        history_.push_back({sim_time_us_, "READY_ASSERTED", e.task, e.worker});

        const double begin = std::max(sim_time_us_, spi_free_at_);

        spi_free_at_ = begin + result_transfer_us();

        w.state = WorkerState::ResultTransfer;

        schedule(EventType::ResultStart, begin, e.worker, e.task);

        schedule(EventType::ResultDone, spi_free_at_, e.worker, e.task);

        break;
    }

    case EventType::ResultStart:
        history_.push_back({sim_time_us_, "SPI_RESULT_TRANSFER_STARTED", e.task, e.worker});
        break;

    case EventType::ResultDone:
        history_.push_back({sim_time_us_, "RESULT_VALIDATED", e.task, e.worker});

        schedule(EventType::AckSent, sim_time_us_, e.worker, e.task);
        break;

    case EventType::AckSent:
        history_.push_back({sim_time_us_, "ACK_SENT", e.task, e.worker});

        schedule(EventType::WorkerIdle, sim_time_us_, e.worker, e.task);
        break;

    case EventType::WorkerIdle:
        t.state = "completed";
        w.completed++;
        w.task = -1;
        w.state = w.enabled ? WorkerState::Idle : WorkerState::Disabled;

        history_.push_back({sim_time_us_, "WORKER_IDLE", e.task, e.worker});

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

    return {{"job_state",
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
            {"workers", ws},
            {"tasks", ts},
            {"events", ev}};
}

} // namespace hetero