cmake_minimum_required(VERSION 3.18)

project(MDVWB VERSION 1.2.0 LANGUAGES CXX)

set(MDVWB_DRIVER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/driver")
set(MDVWB_DRIVER_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver")
set(MDVWB_MANAGER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/manager")
set(MDVWB_MANAGER_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/manager")
set(MDVWB_SCHEDULER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/scheduler")
set(MDVWB_SCHEDULER_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/scheduler")

set(MDVWB_DRIVER_SOURCES
    ${MDVWB_DRIVER_DIR}/MDVWB.cpp
    ${MDVWB_DRIVER_DIR}/MDVWB.h
    ${MDVWB_DRIVER_DIR}/mdv_protocol.cpp
    ${MDVWB_DRIVER_DIR}/mdv_protocol.h
    ${MDVWB_DRIVER_DIR}/mdv_device.cpp
    ${MDVWB_DRIVER_DIR}/mdv_device.h
    ${MDVWB_DRIVER_DIR}/mdv_serial.cpp
    ${MDVWB_DRIVER_DIR}/mdv_serial.h
    ${MDVWB_DRIVER_DIR}/mdv_driver.cpp
    ${MDVWB_DRIVER_DIR}/mdv_driver.h
    ${MDVWB_DRIVER_DIR}/mdv_discovery.cpp
    ${MDVWB_DRIVER_DIR}/mdv_discovery.h
    ${MDVWB_DRIVER_DIR}/mdv_mqtt.cpp
    ${MDVWB_DRIVER_DIR}/mdv_mqtt.h
    ${MDVWB_DRIVER_DIR}/mdv_metadata.cpp
    ${MDVWB_DRIVER_DIR}/mdv_metadata.h
    ${MDVWB_DRIVER_DIR}/mdv_mosquitto.cpp
    ${MDVWB_DRIVER_DIR}/mdv_mosquitto.h
    ${MDVWB_DRIVER_DIR}/mdv_config.cpp
    ${MDVWB_DRIVER_DIR}/mdv_config.h
)

add_executable(MDVWB ${MDVWB_DRIVER_SOURCES})
target_compile_features(MDVWB PRIVATE cxx_std_20)
target_compile_definitions(MDVWB PRIVATE MDVWB_VERSION="${PROJECT_VERSION}")
target_include_directories(MDVWB PRIVATE ${MDVWB_DRIVER_DIR})

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

add_library(mdvwb_mosquitto_transport STATIC
    ${MDVWB_DRIVER_DIR}/mdv_mosquitto.cpp
    ${MDVWB_DRIVER_DIR}/mdv_mosquitto.h
)
target_compile_features(mdvwb_mosquitto_transport PUBLIC cxx_std_20)
target_include_directories(mdvwb_mosquitto_transport PUBLIC ${MDVWB_DRIVER_DIR})
target_link_libraries(mdvwb_mosquitto_transport PUBLIC Threads::Threads)
if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
    target_compile_definitions(mdvwb_mosquitto_transport PRIVATE MDVWB_HAS_MOSQUITTO=1)
    target_include_directories(mdvwb_mosquitto_transport PRIVATE ${MOSQUITTO_INCLUDE_DIR})
    target_link_libraries(mdvwb_mosquitto_transport PRIVATE ${MOSQUITTO_LIBRARY})
endif()

add_library(mdvwb_buses_config STATIC
    ${MDVWB_MANAGER_DIR}/mdv_buses_config.cpp
    ${MDVWB_MANAGER_DIR}/mdv_buses_config.h
)
target_compile_features(mdvwb_buses_config PUBLIC cxx_std_20)
target_include_directories(mdvwb_buses_config PUBLIC ${MDVWB_MANAGER_DIR})

add_library(mdvwb_dashboard_config STATIC
    ${MDVWB_MANAGER_DIR}/mdv_dashboard_config.cpp
    ${MDVWB_MANAGER_DIR}/mdv_dashboard_config.h
)
target_compile_features(mdvwb_dashboard_config PUBLIC cxx_std_20)
target_include_directories(mdvwb_dashboard_config PUBLIC ${MDVWB_MANAGER_DIR})
target_link_libraries(mdvwb_dashboard_config PUBLIC mdvwb_buses_config)

add_library(mdvwb_schedules_config STATIC
    ${MDVWB_MANAGER_DIR}/mdv_schedules_config.cpp
    ${MDVWB_MANAGER_DIR}/mdv_schedules_config.h
)
target_compile_features(mdvwb_schedules_config PUBLIC cxx_std_20)
target_include_directories(mdvwb_schedules_config PUBLIC ${MDVWB_MANAGER_DIR})
target_link_libraries(mdvwb_schedules_config PUBLIC mdvwb_buses_config mdvwb_dashboard_config)


add_library(mdvwb_dashboard_upload STATIC
    ${MDVWB_MANAGER_DIR}/mdvwb_dashboard_upload.cpp
    ${MDVWB_MANAGER_DIR}/mdvwb_dashboard_upload.h
)
target_compile_features(mdvwb_dashboard_upload PUBLIC cxx_std_20)
target_include_directories(mdvwb_dashboard_upload PUBLIC ${MDVWB_MANAGER_DIR})

add_library(mdvwb_service_sync STATIC
    ${MDVWB_MANAGER_DIR}/mdvwb_service_sync.cpp
    ${MDVWB_MANAGER_DIR}/mdvwb_service_sync.h
)
target_compile_features(mdvwb_service_sync PUBLIC cxx_std_20)
target_include_directories(mdvwb_service_sync PUBLIC ${MDVWB_MANAGER_DIR})
target_link_libraries(mdvwb_service_sync PUBLIC mdvwb_buses_config)

add_library(mdvwb_discovery_runner STATIC
    ${MDVWB_MANAGER_DIR}/mdvwb_discovery_runner.cpp
    ${MDVWB_MANAGER_DIR}/mdvwb_discovery_runner.h
)
target_compile_features(mdvwb_discovery_runner PUBLIC cxx_std_20)
target_include_directories(mdvwb_discovery_runner PUBLIC ${MDVWB_MANAGER_DIR})

add_library(mdvwb_manager_mqtt STATIC
    ${MDVWB_MANAGER_DIR}/mdvwb_manager_mqtt.cpp
    ${MDVWB_MANAGER_DIR}/mdvwb_manager_mqtt.h
)
target_compile_features(mdvwb_manager_mqtt PUBLIC cxx_std_20)
target_include_directories(mdvwb_manager_mqtt PUBLIC ${MDVWB_MANAGER_DIR} ${MDVWB_DRIVER_DIR})
target_link_libraries(mdvwb_manager_mqtt
    PUBLIC mdvwb_buses_config mdvwb_dashboard_config mdvwb_schedules_config
           mdvwb_dashboard_upload mdvwb_service_sync mdvwb_mosquitto_transport
           mdvwb_discovery_runner)

add_library(mdvwb_manager_cli STATIC
    ${MDVWB_MANAGER_DIR}/mdvwb_manager_cli.cpp
    ${MDVWB_MANAGER_DIR}/mdvwb_manager_cli.h
    ${MDVWB_MANAGER_DIR}/mdvwb_migration.cpp
    ${MDVWB_MANAGER_DIR}/mdvwb_migration.h
)
target_compile_features(mdvwb_manager_cli PUBLIC cxx_std_20)
target_include_directories(mdvwb_manager_cli PUBLIC ${MDVWB_MANAGER_DIR})
target_link_libraries(mdvwb_manager_cli PUBLIC mdvwb_buses_config mdvwb_service_sync mdvwb_manager_mqtt)

add_executable(mdvwb-manager
    ${MDVWB_MANAGER_DIR}/mdvwb_manager_main.cpp
)
target_link_libraries(mdvwb-manager PRIVATE mdvwb_manager_cli)

add_library(mdvwb_scheduler STATIC
    ${MDVWB_SCHEDULER_DIR}/mdvwb_scheduler.cpp
    ${MDVWB_SCHEDULER_DIR}/mdvwb_scheduler.h
)
target_compile_features(mdvwb_scheduler PUBLIC cxx_std_20)
target_include_directories(mdvwb_scheduler
    PUBLIC ${MDVWB_SCHEDULER_DIR} ${MDVWB_MANAGER_DIR} ${MDVWB_DRIVER_DIR})
target_link_libraries(mdvwb_scheduler
    PUBLIC mdvwb_schedules_config mdvwb_mosquitto_transport)

add_executable(mdvwb-scheduler
    ${MDVWB_SCHEDULER_DIR}/mdvwb_scheduler_main.cpp
)
target_link_libraries(mdvwb-scheduler PRIVATE mdvwb_scheduler)

add_executable(mdvwb_mqtt_delivery_test
    ${MDVWB_DRIVER_TEST_DIR}/mdv_mqtt_delivery_test.cpp
)
target_link_libraries(mdvwb_mqtt_delivery_test PRIVATE mdvwb_mosquitto_transport)

add_executable(mdvwb_buses_config_test
    ${MDVWB_MANAGER_TEST_DIR}/mdv_buses_config_test.cpp
)
target_link_libraries(mdvwb_buses_config_test PRIVATE mdvwb_buses_config)

add_executable(mdvwb_dashboard_config_test
    ${MDVWB_MANAGER_TEST_DIR}/mdv_dashboard_config_test.cpp
)
target_link_libraries(mdvwb_dashboard_config_test PRIVATE mdvwb_dashboard_config)

add_executable(mdvwb_schedules_config_test
    ${MDVWB_MANAGER_TEST_DIR}/mdv_schedules_config_test.cpp
)
target_link_libraries(mdvwb_schedules_config_test PRIVATE mdvwb_schedules_config)

add_executable(mdvwb_dashboard_upload_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_dashboard_upload_test.cpp
)
target_link_libraries(mdvwb_dashboard_upload_test PRIVATE mdvwb_dashboard_upload)

add_executable(mdvwb_manager_cli_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_manager_cli_test.cpp
)
target_link_libraries(mdvwb_manager_cli_test PRIVATE mdvwb_manager_cli)

add_executable(mdvwb_service_sync_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_service_sync_test.cpp
)
target_link_libraries(mdvwb_service_sync_test PRIVATE mdvwb_service_sync)

add_executable(mdvwb_manager_mqtt_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_manager_mqtt_test.cpp
)
target_link_libraries(mdvwb_manager_mqtt_test PRIVATE mdvwb_manager_mqtt)

add_executable(mdvwb_discovery_runner_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_discovery_runner_test.cpp
)
target_link_libraries(mdvwb_discovery_runner_test PRIVATE mdvwb_discovery_runner)

add_executable(mdvwb_migration_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_migration_test.cpp
)
target_link_libraries(mdvwb_migration_test PRIVATE mdvwb_manager_cli)

add_executable(mdvwb_scheduler_test
    ${MDVWB_SCHEDULER_TEST_DIR}/mdvwb_scheduler_test.cpp
)
target_link_libraries(mdvwb_scheduler_test PRIVATE mdvwb_scheduler)

set(MDVWB_PROJECT_TARGETS
    MDVWB
    mdvwb_mosquitto_transport
    mdvwb_mqtt_delivery_test
    mdvwb_buses_config
    mdvwb_dashboard_config
    mdvwb_schedules_config
    mdvwb_dashboard_upload
    mdvwb_service_sync
    mdvwb_discovery_runner
    mdvwb_manager_mqtt
    mdvwb_manager_cli
    mdvwb-manager
    mdvwb_scheduler
    mdvwb-scheduler
    mdvwb_buses_config_test
    mdvwb_dashboard_config_test
    mdvwb_schedules_config_test
    mdvwb_dashboard_upload_test
    mdvwb_manager_cli_test
    mdvwb_service_sync_test
    mdvwb_manager_mqtt_test
    mdvwb_discovery_runner_test
    mdvwb_migration_test
    mdvwb_scheduler_test
)

if(MSVC)
    foreach(target IN LISTS MDVWB_PROJECT_TARGETS)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endforeach()
else()
    foreach(target IN LISTS MDVWB_PROJECT_TARGETS)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endforeach()
endif()

source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${MDVWB_DRIVER_SOURCES})

enable_testing()
add_test(NAME mdv_protocol_self_test COMMAND MDVWB --self-test)
add_test(NAME mdvwb_mqtt_delivery_test COMMAND mdvwb_mqtt_delivery_test)
add_test(NAME mdv_buses_config_test COMMAND mdvwb_buses_config_test)
add_test(NAME mdvwb_dashboard_config_test COMMAND mdvwb_dashboard_config_test)
add_test(NAME mdvwb_schedules_config_test COMMAND mdvwb_schedules_config_test)
add_test(NAME mdvwb_dashboard_upload_test COMMAND mdvwb_dashboard_upload_test)
add_test(NAME mdvwb_manager_cli_test COMMAND mdvwb_manager_cli_test)
add_test(NAME mdvwb_service_sync_test COMMAND mdvwb_service_sync_test)
add_test(NAME mdvwb_manager_mqtt_test COMMAND mdvwb_manager_mqtt_test)
add_test(NAME mdvwb_discovery_runner_test COMMAND mdvwb_discovery_runner_test)
add_test(NAME mdvwb_migration_test COMMAND mdvwb_migration_test)
add_test(NAME mdvwb_scheduler_test COMMAND mdvwb_scheduler_test)

include(GNUInstallDirs)
install(TARGETS MDVWB mdvwb-manager mdvwb-scheduler RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
