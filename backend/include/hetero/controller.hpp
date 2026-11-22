#pragma once
#include "simulation_backend.hpp"
namespace hetero
{
class Controller
{
  public:
    Controller();
    nlohmann::json state() const;
    void configure(const nlohmann::json&);
    void start();
    void pause();
    void resume();
    void reset();
    void set_policy(const nlohmann::json&);
    void set_worker(const nlohmann::json&);

  private:
    SimulationBackend backend_;
};
} // namespace hetero
