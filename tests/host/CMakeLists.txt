cmake_minimum_required(VERSION 3.20)
project(pulse_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(
    pulse_host_tests
    ../../src/Pulse.cpp
    freertos_stubs.cpp
    test_pulse.cpp
)

target_include_directories(
    pulse_host_tests
    PRIVATE
        include
        ../../src
)

target_compile_options(
    pulse_host_tests
    PRIVATE
        -Wall
        -Wextra
        -Werror
)

target_link_libraries(pulse_host_tests PRIVATE pthread)
