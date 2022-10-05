#include "logger.hpp"
#include "common.hpp"
#include <cstdlib>

namespace SNL1 {

void panic(const std::string &msg) {
    Logger::global->log(LOG_CRITICAL, msg);
    std::abort();
}

}
