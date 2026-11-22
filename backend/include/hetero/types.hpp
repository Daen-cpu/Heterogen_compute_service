#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hetero
{
enum class Policy
{
    Predicted,
    LeastLoaded,
    RoundRobin
};
enum class WorkerState
{
    Disabled,
    Idle,
    InputTransfer,
    Computing,
    Ready,
    ResultTransfer,
    Error
};
enum class JobState
{
    Idle,
    Running,
    Paused,
    Completed,
    Failed
};
inline std::string to_string(Policy p)
{
    switch (p)
    {
    case Policy::Predicted:
        return "predicted";
    case Policy::LeastLoaded:
        return "least_loaded";
    case Policy::RoundRobin:
        return "round_robin";
    }
    return "predicted";
}
inline std::string to_string(WorkerState s)
{
    switch (s)
    {
    case WorkerState::Disabled:
        return "disabled";
    case WorkerState::Idle:
        return "idle";
    case WorkerState::InputTransfer:
        return "input_transfer";
    case WorkerState::Computing:
        return "computing";
    case WorkerState::Ready:
        return "ready";
    case WorkerState::ResultTransfer:
        return "result_transfer";
    case WorkerState::Error:
        return "error";
    }
    return "unknown";
}
inline std::string to_string(JobState s)
{
    switch (s)
    {
    case JobState::Idle:
        return "idle";
    case JobState::Running:
        return "running";
    case JobState::Paused:
        return "paused";
    case JobState::Completed:
        return "completed";
    case JobState::Failed:
        return "failed";
    }
    return "unknown";
}
struct Config
{
    std::uint32_t matrix_size{128};
    std::uint32_t tile_size{32};
    std::uint32_t worker_count{3};
    double spi_mbps{10.0};
    double wall_delay_ms{4.0};
};
struct WorkerView
{
    std::uint32_t id{};
    WorkerState state{WorkerState::Idle};
    double speed_factor{1.0};
    std::uint64_t completed_tiles{};
    double busy_us{};
    double wait_us{};
    std::optional<std::uint32_t> task_id;
};
struct TaskView
{
    std::uint32_t id{}, tile_row{}, tile_col{};
    std::string state;
    std::optional<std::uint32_t> worker_id;
};
struct EventView
{
    double time_us{};
    std::string type;
    std::uint32_t task_id{}, worker_id{};
};
} // namespace hetero
