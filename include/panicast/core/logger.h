// Logging system: singleton Logger, writes to ~/.local/share/panicast/panicast.log
#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace panicast
{

class Logger {
public:
    static Logger &instance();

    void init();
    void log(const std::string &msg);
    ~Logger();

private:
    Logger() = default;
    std::ofstream file_;
    std::mutex mtx_;
};

} // namespace panicast

#define LOG(msg) ::panicast::Logger::instance().log(msg)
