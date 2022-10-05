cmake_minimum_required(VERSION 3.22)
project(simple_net_lib)

set(CMAKE_CXX_STANDARD 20)

include_directories(include)

set(LIB_SRC
        src/common.cpp include/common.hpp
        src/io_context.cpp include/io_context.hpp src/io_context.imp.hpp
        include/tcp_socket.hpp src/tcp_socket.cpp
        src/logger.cpp include/logger.hpp
        )

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")

add_library(simple_net_lib ${LIB_SRC})

add_executable(demo_tcp_echo_server demo/tcp_echo_server.cpp)

add_executable(demo_tcp_hello_server demo/tcp_hello_server.cpp)

target_link_libraries(demo_tcp_echo_server simple_net_lib)
target_link_libraries(demo_tcp_hello_server simple_net_lib)
