#include "hetero/simulation_backend.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace
{

using namespace hetero;
using Json = nlohmann::json;

constexpr double EPSILON = 1e-6;

Config make_config()
{
    Config config;
    config.matrix_size = 64;
    config.tile_size = 32;
    config.worker_count = 3;
    config.spi_mbps = 100.0;
    config.wall_delay_ms = 0.0;
    return config;
}

Json run_simulation(Policy policy, bool disable_worker_1 = false)
{
    SimulationBackend backend;

    backend.configure(make_config());
    backend.set_policy(policy);

    if (disable_worker_1)
    {
        backend.set_worker_enabled(1, false);
    }

    backend.start();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);

    Json state;

    while (std::chrono::steady_clock::now() < deadline)
    {
        state = backend.snapshot();

        if (state.at("job_state") == "completed")
        {
            return state;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ADD_FAILURE() << "Simulation did not finish within timeout";

    return backend.snapshot();
}

std::vector<Json> events_of_type(
    const Json& state,
    const std::string& type)
{
    std::vector<Json> result;

    for (const auto& event : state.at("events"))
    {
        if (event.at("type") == type)
        {
            result.push_back(event);
        }
    }

    return result;
}

std::vector<std::uint32_t> dispatched_workers(const Json& state)
{
    std::vector<std::uint32_t> result;

    for (const auto& event : state.at("events"))
    {
        if (event.at("type") == "TASK_DISPATCHED")
        {
            result.push_back(event.at("worker_id").get<std::uint32_t>());
        }
    }

    return result;
}

struct Interval
{
    double start;
    double end;
};

std::vector<Interval> collect_intervals(
    const Json& state,
    const std::string& start_event,
    const std::string& end_event)
{
    using Key = std::pair<std::uint32_t, std::uint32_t>;

    std::map<Key, double> starts;
    std::vector<Interval> intervals;

    for (const auto& event : state.at("events"))
    {
        const auto type = event.at("type").get<std::string>();

        const auto worker =
            event.at("worker_id").get<std::uint32_t>();

        const auto task =
            event.at("task_id").get<std::uint32_t>();

        const Key key{worker, task};

        const double time =
            event.at("time_us").get<double>();

        if (type == start_event)
        {
            starts[key] = time;
        }
        else if (type == end_event)
        {
            const auto it = starts.find(key);

            if (it != starts.end())
            {
                intervals.push_back({it->second, time});
                starts.erase(it);
            }
        }
    }

    return intervals;
}

bool has_overlapping_intervals(
    const std::vector<Interval>& intervals)
{
    for (std::size_t i = 0; i < intervals.size(); ++i)
    {
        for (std::size_t j = i + 1; j < intervals.size(); ++j)
        {
            const bool overlap =
                intervals[i].start < intervals[j].end - EPSILON &&
                intervals[j].start < intervals[i].end - EPSILON;

            if (overlap)
            {
                return true;
            }
        }
    }

    return false;
}

bool has_serial_intervals(
    std::vector<Interval> intervals)
{
    std::sort(
        intervals.begin(),
        intervals.end(),
        [](const Interval& lhs, const Interval& rhs)
        {
            return lhs.start < rhs.start;
        });

    for (std::size_t i = 1; i < intervals.size(); ++i)
    {
        if (intervals[i].start < intervals[i - 1].end - EPSILON)
        {
            return false;
        }
    }

    return true;
}

void expect_all_tasks_completed(const Json& state)
{
    EXPECT_EQ(state.at("job_state"), "completed");

    EXPECT_EQ(
        state.at("completed_tasks").get<std::size_t>(),
        state.at("total_tasks").get<std::size_t>());

    for (const auto& task : state.at("tasks"))
    {
        EXPECT_EQ(task.at("state"), "completed");
        EXPECT_NE(task.at("worker_id"), nullptr);
    }

    for (const auto& worker : state.at("workers"))
    {
        const auto worker_state = worker.at("state").get<std::string>();

        EXPECT_TRUE(
            worker_state == "IDLE" ||
            worker_state == "FAILED");
        EXPECT_EQ(worker.at("task_id"), nullptr);
    }
}

TEST(SchedulerRoundRobin, AssignsInitialTasksCyclically)
{
    const Json state = run_simulation(Policy::RoundRobin);

    const auto workers = dispatched_workers(state);

    ASSERT_GE(workers.size(), 3u);

    EXPECT_EQ(workers[0], 0u);
    EXPECT_EQ(workers[1], 1u);
    EXPECT_EQ(workers[2], 2u);

    expect_all_tasks_completed(state);
}

TEST(SchedulerRoundRobin, SkipsDisabledWorker)
{
    const Json state =
        run_simulation(Policy::RoundRobin, true);

    const auto workers = dispatched_workers(state);

    ASSERT_GE(workers.size(), 2u);

    EXPECT_EQ(workers[0], 0u);
    EXPECT_EQ(workers[1], 2u);

    for (const auto worker_id : workers)
    {
        EXPECT_NE(worker_id, 1u);
    }

    const auto& worker = state.at("workers").at(1);

    EXPECT_EQ(worker.at("state"), "FAILED");

    expect_all_tasks_completed(state);
}

TEST(SchedulerLeastLoaded, CompletesAllTasks)
{
    const Json state =
        run_simulation(Policy::LeastLoaded);

    expect_all_tasks_completed(state);
}

TEST(SchedulerLeastLoaded, PrefersFastestWorkerInitially)
{
    const Json state =
        run_simulation(Policy::LeastLoaded);

    const auto workers = dispatched_workers(state);

    ASSERT_FALSE(workers.empty());

    EXPECT_EQ(workers.front(), 2u);
}

TEST(SchedulerPredicted, CompletesAllTasks)
{
    const Json state =
        run_simulation(Policy::Predicted);

    expect_all_tasks_completed(state);
}

TEST(SchedulerPredicted, PrefersFastestWorkerInitially)
{
    const Json state =
        run_simulation(Policy::Predicted);

    const auto workers = dispatched_workers(state);

    ASSERT_FALSE(workers.empty());

    EXPECT_EQ(workers.front(), 2u);
}

TEST(SchedulerIntegration, EveryTaskIsDispatchedExactlyOnce)
{
    const Json state =
        run_simulation(Policy::Predicted);

    const auto dispatched =
        events_of_type(state, "TASK_DISPATCHED");

    const auto completed =
        events_of_type(state, "WORKER_IDLE");

    ASSERT_EQ(dispatched.size(), state.at("total_tasks").get<std::size_t>());
    ASSERT_EQ(completed.size(), state.at("total_tasks").get<std::size_t>());

    std::set<std::uint32_t> dispatched_task_ids;
    std::set<std::uint32_t> completed_task_ids;

    for (const auto& event : dispatched)
    {
        const auto task_id =
            event.at("task_id").get<std::uint32_t>();

        EXPECT_TRUE(dispatched_task_ids.insert(task_id).second);
    }

    for (const auto& event : completed)
    {
        const auto task_id =
            event.at("task_id").get<std::uint32_t>();

        EXPECT_TRUE(completed_task_ids.insert(task_id).second);
    }

    EXPECT_EQ(dispatched_task_ids, completed_task_ids);
}

TEST(SchedulerIntegration, SharedSpiTransfersDoNotOverlap)
{
    const Json state =
        run_simulation(Policy::Predicted);

    auto input_intervals = collect_intervals(
        state,
        "SPI_INPUT_TRANSFER_STARTED",
        "SPI_INPUT_TRANSFER_COMPLETED");

    auto result_intervals = collect_intervals(
        state,
        "SPI_RESULT_TRANSFER_STARTED",
        "RESULT_VALIDATED");

    input_intervals.insert(
        input_intervals.end(),
        result_intervals.begin(),
        result_intervals.end());

    ASSERT_FALSE(input_intervals.empty());

    EXPECT_TRUE(has_serial_intervals(input_intervals));
}

TEST(SchedulerIntegration, ComputationsCanOverlap)
{
    const Json state =
        run_simulation(Policy::Predicted);

    const auto compute_intervals = collect_intervals(
        state,
        "COMPUTATION_STARTED",
        "COMPUTATION_COMPLETED");

    ASSERT_GE(compute_intervals.size(), 2u);

    EXPECT_TRUE(has_overlapping_intervals(compute_intervals));
}

TEST(SchedulerIntegration, EveryTaskFollowsExpectedLifecycle)
{
    const Json state =
        run_simulation(Policy::Predicted);

    const auto expected_events = {
        "TASK_DISPATCHED",
        "SPI_INPUT_TRANSFER_STARTED",
        "SPI_INPUT_TRANSFER_COMPLETED",
        "COMPUTATION_STARTED",
        "COMPUTATION_COMPLETED",
        "READY_ASSERTED",
        "SPI_RESULT_TRANSFER_STARTED",
        "RESULT_VALIDATED",
        "ACK_SENT",
        "READY_DEASSERTED",
        "WORKER_IDLE"
    };

    for (const auto& task : state.at("tasks"))
    {
        const auto task_id =
            task.at("id").get<std::uint32_t>();

        double previous_time = -1.0;

        for (const auto* event_type : expected_events)
        {
            bool found = false;

            for (const auto& event : state.at("events"))
            {
                if (event.at("task_id").get<std::uint32_t>() != task_id ||
                    event.at("type") != event_type)
                {
                    continue;
                }

                const double current_time =
                    event.at("time_us").get<double>();

                EXPECT_GE(current_time, previous_time);

                previous_time = current_time;
                found = true;
                break;
            }

            EXPECT_TRUE(found)
                << "Missing event " << event_type
                << " for task " << task_id;
        }
    }
}

TEST(SchedulerIntegration, AllPoliciesFinishSameWorkload)
{
    const auto rr = run_simulation(Policy::RoundRobin);
    const auto ll = run_simulation(Policy::LeastLoaded);
    const auto predicted = run_simulation(Policy::Predicted);

    expect_all_tasks_completed(rr);
    expect_all_tasks_completed(ll);
    expect_all_tasks_completed(predicted);

    EXPECT_EQ(rr.at("total_tasks"), ll.at("total_tasks"));
    EXPECT_EQ(rr.at("total_tasks"), predicted.at("total_tasks"));
}

} // namespace