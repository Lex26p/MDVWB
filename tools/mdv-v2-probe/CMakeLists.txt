cmake_minimum_required(VERSION 3.18)
project(mdv_v2_probe LANGUAGES CXX)

# Temporary research tool; deliberately outside the production package.
set(DRIVER "${CMAKE_CURRENT_LIST_DIR}/../../src/driver")
add_executable(mdv-v2-probe probe.cpp
    "${DRIVER}/mdv_protocol.cpp"
    "${DRIVER}/mdv_serial.cpp"
    "${DRIVER}/serial_port.cpp")
target_include_directories(mdv-v2-probe PRIVATE "${DRIVER}")
target_compile_features(mdv-v2-probe PRIVATE cxx_std_20)
target_compile_options(mdv-v2-probe PRIVATE -Wall -Wextra -Wpedantic)
find_package(Threads REQUIRED)
target_link_libraries(mdv-v2-probe PRIVATE Threads::Threads)
option(PROBE_STATIC "Build a standalone Linux executable" OFF)
if(PROBE_STATIC)
    target_link_options(mdv-v2-probe PRIVATE -static)
endif()

include(CTest)
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME probe_serial
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_probe.py"
                "$<TARGET_FILE:mdv-v2-probe>")
endif()
