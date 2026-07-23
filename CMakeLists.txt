cmake_minimum_required(VERSION 3.18)

project(MDVWB VERSION 1.0.0 LANGUAGES CXX)

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
    mdv_mosquitto.cpp
    mdv_mosquitto.h
    mdv_config.cpp
    mdv_config.h
)

target_compile_features(MDVWB PRIVATE cxx_std_20)
target_compile_definitions(MDVWB PRIVATE MDVWB_VERSION="${PROJECT_VERSION}")

option(MDVWB_REQUIRE_MOSQUITTO "Fail configuration when libmosquitto is unavailable" OFF)

find_package(Threads REQUIRED)
target_link_libraries(MDVWB PRIVATE Threads::Threads)

# libmosquitto is optional for local protocol work. When available, the same
# executable gains a real asynchronous MQTT client without changing sources.
find_path(MOSQUITTO_INCLUDE_DIR NAMES mosquitto.h)
find_library(MOSQUITTO_LIBRARY NAMES mosquitto libmosquitto)
if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
    target_compile_definitions(MDVWB PRIVATE MDVWB_HAS_MOSQUITTO=1)
    target_include_directories(MDVWB PRIVATE ${MOSQUITTO_INCLUDE_DIR})
    target_link_libraries(MDVWB PRIVATE ${MOSQUITTO_LIBRARY})
    message(STATUS "MDVWB: libmosquitto support enabled")
else()
    if(MDVWB_REQUIRE_MOSQUITTO)
        message(FATAL_ERROR
            "MDVWB: libmosquitto is required for this build. Install libmosquitto-dev and configure again.")
    endif()
    message(STATUS "MDVWB: libmosquitto not found; protocol self-test remains available")
endif()

if(MSVC)
    target_compile_options(MDVWB PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(MDVWB PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME mdv_protocol_self_test COMMAND MDVWB --self-test)

include(GNUInstallDirs)
install(TARGETS MDVWB RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
