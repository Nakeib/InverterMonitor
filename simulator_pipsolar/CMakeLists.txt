cmake_minimum_required(VERSION 3.8)
project(simulator_modbus VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(simulator_modbus main.cpp)

if(WIN32)
  target_compile_definitions(simulator_modbus PRIVATE WINDOWS_SERIAL)
endif()
