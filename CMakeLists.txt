cmake_minimum_required(VERSION 3.20)

project(MDVWB VERSION 0.6.0 LANGUAGES CXX)

add_executable(MDVWB
    MDVWB.cpp
    MDVWB.h
    mdv_protocol.cpp
    mdv_protocol.h
    mdv_device.cpp
    mdv_device.h
    mdv_serial.cpp
    mdv_serial.h
    mdv_driver.cpp
    mdv_driver.h
    mdv_mqtt.cpp
    mdv_mqtt.h
)

target_compile_features(MDVWB PRIVATE cxx_std_20)

find_package(Threads REQUIRED)
target_link_libraries(MDVWB PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(MDVWB PRIVATE /W4 /permissive-)
else()
    target_compile_options(MDVWB PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME mdv_protocol_self_test COMMAND MDVWB --self-test)
