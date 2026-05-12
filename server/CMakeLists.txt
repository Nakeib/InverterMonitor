cmake_minimum_required(VERSION 3.10)
project(inverter_monitor_server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(inverter_monitor_server main.cpp)

target_compile_options(inverter_monitor_server PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(inverter_monitor_server PRIVATE pthread)
