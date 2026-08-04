// Logging system: singleton Logger, writes to ~/.local/share/podradio/podradio.log
#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace podradio
{

class Logger {
public:
    static Logger& instance();

    void init();
    void log(const std::string& msg);
    ~Logger();

private:
    Logger() = default;
    std::ofstream file_;
    std::mutex mtx_;
};

} // namespace podradio

#define LOG(msg) ::podradio::Logger::instance().log(msg)
