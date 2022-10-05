#ifndef SIMPLE_NET_LIB_COMMON_HPP
#define SIMPLE_NET_LIB_COMMON_HPP

#include <string>

namespace SNL1 {

struct DisableCopy {

    DisableCopy() = default;

    DisableCopy(const DisableCopy &) = delete;

    DisableCopy &operator=(DisableCopy) = delete;

};

void panic(const std::string &msg);

}

#endif //SIMPLE_NET_LIB_COMMON_HPP
