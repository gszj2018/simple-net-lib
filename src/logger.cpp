#include "logger.hpp"
#include <cstdio>
#include <memory>


namespace SNL1 {

void Logger::log(LogLevel level, const std::string &msg) {
    std::fprintf(stderr, "[%s] %s\n", LEVEL_NAME[level], msg.c_str());
}

Logger::~Logger() = default;

std::unique_ptr<Logger> Logger::global = std::make_unique<Logger>();

}