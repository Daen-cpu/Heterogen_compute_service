#pragma once
#include "execution_backend.hpp"
#include <memory>
#include <string>

namespace hetero
{
class Controller
{
public:
    Controller();
    explicit Controller(std::unique_ptr<IExecutionBackend> backend);
    nlohmann::json state() const;
    std::string export_log() const;
    void configure(const nlohmann::json&);
    void start();
    void pause();
    void resume();
    void reset();
    void set_policy(const nlohmann::json&);
    void set_worker(const nlohmann::json&);

private:
    std::unique_ptr<IExecutionBackend> backend_;
};
} // namespace hetero
