cmake_minimum_required(VERSION 3.10)
project(Concurrent_Servers C)

set(CMAKE_C_STANDARD 99)

# Part 1: Sequential Server
add_executable(sequential_server sequential_server.c)

if(WIN32)
    target_link_libraries(sequential_server ws2_32)
endif()
