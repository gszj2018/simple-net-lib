#ifndef SIMPLE_NET_LIB_LOGGER_HPP
#define SIMPLE_NET_LIB_LOGGER_HPP

#include <string>
#include <memory>

namespace SNL1 {

enum LogLevel {
    LOG_CRITICAL = 0,
    LOG_ERROR = 1,
    LOG_WARN = 2,
    LOG_INFO = 3
};

constexpr const char *LEVEL_NAME[] = {"CRIT", "ERR", "WARN", "INFO"};

class Logger {
public:
    virtual void log(LogLevel level, const std::string &msg);

    virtual ~Logger();

    static std::unique_ptr<Logger> global;
};


}

#endif //SIMPLE_NET_LIB_LOGGER_HPP
