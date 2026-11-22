#include "hetero/controller.hpp"
#include <stdexcept>
namespace hetero
{
Controller::Controller() = default;
nlohmann::json Controller::state() const
{
    return backend_.snapshot();
}
void Controller::configure(const nlohmann::json& j)
{
    Config c;
    c.matrix_size = j.value("matrix_size", 128u);
    c.tile_size = j.value("tile_size", 32u);
    c.worker_count = j.value("worker_count", 3u);
    c.spi_mbps = j.value("spi_mbps", 10.0);
    c.wall_delay_ms = j.value("wall_delay_ms", 4.0);
    backend_.configure(c);
}
void Controller::start()
{
    backend_.start();
}
void Controller::pause()
{
    backend_.pause();
}
void Controller::resume()
{
    backend_.resume();
}
void Controller::reset()
{
    backend_.reset();
}
void Controller::set_policy(const nlohmann::json& j)
{
    auto p = j.value("policy", std::string("predicted"));
    if (p == "predicted")
        backend_.set_policy(Policy::Predicted);
    else if (p == "least_loaded")
        backend_.set_policy(Policy::LeastLoaded);
    else if (p == "round_robin")
        backend_.set_policy(Policy::RoundRobin);
    else
        throw std::invalid_argument("unknown policy");
}
void Controller::set_worker(const nlohmann::json& j)
{
    backend_.set_worker_enabled(j.at("id").get<std::uint32_t>(), j.at("enabled").get<bool>());
}
} // namespace hetero
