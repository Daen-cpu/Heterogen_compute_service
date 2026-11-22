#pragma once
#include "execution_backend.hpp"
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

namespace hetero
{
class SimulationBackend final : public IExecutionBackend
{
  public:
    SimulationBackend();
    ~SimulationBackend() override;
    void configure(const Config&) override;
    void start() override;
    void pause() override;
    void resume() override;
    void reset() override;
    void set_policy(Policy) override;
    void set_worker_enabled(std::uint32_t, bool) override;
    nlohmann::json snapshot() const override;

  private:
    enum class EventType
    {
        InputStart,
        InputDone,
        ComputeDone,
        ReadyAsserted,
        ResultStart,
        ResultDone,
        AckSent,
        WorkerIdle
    };
    struct Task
    {
        std::uint32_t id{}, row{}, col{};
        std::string state{"queued"};
        int worker{-1};
    };
    struct Worker
    {
        WorkerState state{WorkerState::Idle};
        bool enabled{true};
        double speed{1.0};
        std::uint64_t completed{};
        double busy_us{}, wait_us{};
        double state_since{};
        bool ready_asserted{};
        bool input_transfer_active{};
        int task{-1};
    };
    struct Event
    {
        double at{};
        std::uint64_t seq{};
        EventType type{};
        std::uint32_t worker{}, task{};
    };
    struct Later
    {
        bool operator()(const Event& a, const Event& b) const
        {
            return a.at == b.at ? a.seq > b.seq : a.at > b.at;
        }
    };
    void run();
    void process_one();
    void dispatch();
    void schedule(EventType, double, std::uint32_t, std::uint32_t);
    double input_transfer_us() const;
    double result_transfer_us() const;
    double compute_us(const Worker&) const;
    void initialize_workers(std::uint32_t count);
    int choose_task(std::uint32_t worker) const;
    std::uint32_t choose_worker() const;
    double estimate_completion_time(std::uint32_t worker, std::uint32_t task) const;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    bool stopping_{false}, running_{false}, paused_{false};
    Config config_{};
    Policy policy_{Policy::Predicted};
    std::vector<Worker> workers_;
    std::vector<Task> tasks_;
    std::priority_queue<Event, std::vector<Event>, Later> events_;
    std::vector<EventView> history_;
    double sim_time_us_{0}, spi_free_at_{0};
    std::uint64_t sequence_{0};
    std::uint32_t rr_worker_cursor_{0};
};
} // namespace hetero
