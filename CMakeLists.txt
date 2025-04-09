cmake_minimum_required(VERSION 3.27)
project(TCP_Server)

set(CMAKE_CXX_STANDARD 17)

add_executable(TCP_Server main.cpp server/tcp.cpp libs.h server/udp.cpp ThreadPool.cpp)

target_link_libraries(TCP_Server ws2_32)
