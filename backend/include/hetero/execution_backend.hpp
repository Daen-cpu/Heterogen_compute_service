#pragma once
#include "types.hpp"
#include <memory>
#include <nlohmann/json.hpp>

namespace hetero
{
// Stable boundary: the scheduler/controller does not know whether execution is simulated or
// physical.
class IExecutionBackend
{
  public:
    virtual ~IExecutionBackend() = default;
    virtual void configure(const Config&) = 0;
    virtual void start() = 0;
    virtual void pause() = 0;
    virtual void resume() = 0;
    virtual void reset() = 0;
    virtual void set_policy(Policy) = 0;
    virtual void set_worker_enabled(std::uint32_t id, bool enabled) = 0;
    virtual nlohmann::json snapshot() const = 0;
};
} // namespace hetero
