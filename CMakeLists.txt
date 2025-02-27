cmake_minimum_required(VERSION 3.27)
project(TCP_Server)

set(CMAKE_CXX_STANDARD 17)

add_executable(TCP_Server main.cpp)
add_executable(tst tst.cpp)

target_link_libraries(TCP_Server ws2_32)
target_link_libraries(tst ws2_32)
