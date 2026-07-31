cmake_minimum_required(VERSION 3.18)

project(MDVWB VERSION 1.3.0 LANGUAGES CXX)

set(MDVWB_COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/common")
set(MDVWB_COMMON_TEST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/tests/common")
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

add_library(mdvwb_json STATIC
    ${MDVWB_COMMON_DIR}/json.cpp
    ${MDVWB_COMMON_DIR}/json.h
)
target_compile_features(mdvwb_json PUBLIC cxx_std_20)
target_include_directories(mdvwb_json PUBLIC ${MDVWB_COMMON_DIR})

add_library(mdvwb_serial_port STATIC
    ${MDVWB_DRIVER_DIR}/serial_port.cpp
    ${MDVWB_DRIVER_DIR}/serial_port.h
)
target_compile_features(mdvwb_serial_port PUBLIC cxx_std_20)
target_include_directories(mdvwb_serial_port PUBLIC ${MDVWB_DRIVER_DIR})
target_link_libraries(mdvwb_serial_port PUBLIC Threads::Threads)

target_link_libraries(MDVWB PRIVATE Threads::Threads mdvwb_serial_port)

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

add_library(mdvwb_modbus_rtu STATIC
    ${MDVWB_DRIVER_DIR}/modbus_rtu.cpp
    ${MDVWB_DRIVER_DIR}/modbus_rtu.h
    ${MDVWB_DRIVER_DIR}/modbus_rtu_serial.cpp
    ${MDVWB_DRIVER_DIR}/modbus_rtu_serial.h
)
target_compile_features(mdvwb_modbus_rtu PUBLIC cxx_std_20)
target_include_directories(mdvwb_modbus_rtu PUBLIC ${MDVWB_DRIVER_DIR})
target_link_libraries(mdvwb_modbus_rtu PUBLIC mdvwb_serial_port)

add_library(mdvwb_modbus_profile STATIC
    ${MDVWB_DRIVER_DIR}/modbus_profile.cpp
    ${MDVWB_DRIVER_DIR}/modbus_profile.h
    ${MDVWB_DRIVER_DIR}/modbus_value.cpp
    ${MDVWB_DRIVER_DIR}/modbus_value.h
    ${MDVWB_DRIVER_DIR}/modbus_semantic.cpp
    ${MDVWB_DRIVER_DIR}/modbus_semantic.h
    ${MDVWB_DRIVER_DIR}/modbus_resolver.cpp
    ${MDVWB_DRIVER_DIR}/modbus_resolver.h
    ${MDVWB_DRIVER_DIR}/modbus_scan.cpp
    ${MDVWB_DRIVER_DIR}/modbus_scan.h
)
target_compile_features(mdvwb_modbus_profile PUBLIC cxx_std_20)
target_include_directories(mdvwb_modbus_profile
    PUBLIC ${MDVWB_DRIVER_DIR} ${MDVWB_COMMON_DIR})
target_link_libraries(mdvwb_modbus_profile
    PUBLIC mdvwb_json mdvwb_serial_port)

add_library(mdvwb_modbus_scan STATIC
    ${MDVWB_DRIVER_DIR}/modbus_scan_execute.cpp
    ${MDVWB_DRIVER_DIR}/modbus_scan_execute.h
)
target_compile_features(mdvwb_modbus_scan PUBLIC cxx_std_20)
target_include_directories(mdvwb_modbus_scan PUBLIC ${MDVWB_DRIVER_DIR})
target_link_libraries(mdvwb_modbus_scan
    PUBLIC mdvwb_modbus_profile mdvwb_modbus_rtu)

add_executable(mdvwb-offline
    ${MDVWB_DRIVER_DIR}/mdv_offline.cpp
)
target_compile_features(mdvwb-offline PRIVATE cxx_std_20)
target_include_directories(mdvwb-offline PRIVATE ${MDVWB_DRIVER_DIR})
target_link_libraries(mdvwb-offline PRIVATE mdvwb_mosquitto_transport)

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

add_executable(mdvwb_json_test
    ${MDVWB_COMMON_TEST_DIR}/json_test.cpp
)
target_link_libraries(mdvwb_json_test PRIVATE mdvwb_json)

add_executable(mdvwb_mqtt_delivery_test
    ${MDVWB_DRIVER_TEST_DIR}/mdv_mqtt_delivery_test.cpp
    ${MDVWB_DRIVER_DIR}/mdv_protocol.cpp
    ${MDVWB_DRIVER_DIR}/mdv_device.cpp
)
target_link_libraries(mdvwb_mqtt_delivery_test PRIVATE mdvwb_mosquitto_transport)

add_executable(mdvwb_driver_fairness_test
    ${MDVWB_DRIVER_TEST_DIR}/mdv_driver_fairness_test.cpp
    ${MDVWB_DRIVER_DIR}/mdv_protocol.cpp
    ${MDVWB_DRIVER_DIR}/mdv_device.cpp
    ${MDVWB_DRIVER_DIR}/mdv_driver.cpp
)
target_compile_features(mdvwb_driver_fairness_test PRIVATE cxx_std_20)
target_include_directories(mdvwb_driver_fairness_test PRIVATE ${MDVWB_DRIVER_DIR})

add_executable(mdvwb_modbus_rtu_test
    ${MDVWB_DRIVER_TEST_DIR}/modbus_rtu_test.cpp
)
target_link_libraries(mdvwb_modbus_rtu_test PRIVATE mdvwb_modbus_rtu)

add_executable(mdvwb_modbus_rtu_serial_test
    ${MDVWB_DRIVER_TEST_DIR}/modbus_rtu_serial_test.cpp
)
target_link_libraries(mdvwb_modbus_rtu_serial_test PRIVATE mdvwb_modbus_rtu)

add_executable(mdvwb_modbus_profile_test
    ${MDVWB_DRIVER_TEST_DIR}/modbus_profile_test.cpp
)
target_link_libraries(mdvwb_modbus_profile_test PRIVATE mdvwb_modbus_profile)

add_executable(mdvwb_modbus_value_test
    ${MDVWB_DRIVER_TEST_DIR}/modbus_value_test.cpp
)
target_link_libraries(mdvwb_modbus_value_test PRIVATE mdvwb_modbus_profile)

add_executable(mdvwb_modbus_semantic_test
    ${MDVWB_DRIVER_TEST_DIR}/modbus_semantic_test.cpp
)
target_link_libraries(mdvwb_modbus_semantic_test PRIVATE mdvwb_modbus_profile)

add_executable(mdvwb_modbus_resolver_test
    ${MDVWB_DRIVER_TEST_DIR}/modbus_resolver_test.cpp
)
target_link_libraries(mdvwb_modbus_resolver_test PRIVATE mdvwb_modbus_profile)

add_executable(mdvwb_modbus_scan_test
    ${MDVWB_DRIVER_TEST_DIR}/modbus_scan_test.cpp
)
target_link_libraries(mdvwb_modbus_scan_test PRIVATE mdvwb_modbus_profile)

add_executable(mdvwb_modbus_scan_execute_test
    ${MDVWB_DRIVER_TEST_DIR}/modbus_scan_execute_test.cpp
)
target_link_libraries(mdvwb_modbus_scan_execute_test PRIVATE mdvwb_modbus_scan)

add_executable(mdvwb_modbus_vrf_reference_test
    ${MDVWB_DRIVER_TEST_DIR}/modbus_vrf_reference_test.cpp
)
target_link_libraries(mdvwb_modbus_vrf_reference_test PRIVATE mdvwb_json)
target_compile_definitions(mdvwb_modbus_vrf_reference_test
    PRIVATE MDVWB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

add_executable(mdvwb_modbus_vrf_profile_test
    ${MDVWB_DRIVER_TEST_DIR}/modbus_vrf_profile_test.cpp
)
target_link_libraries(mdvwb_modbus_vrf_profile_test PRIVATE mdvwb_modbus_scan)
target_compile_definitions(mdvwb_modbus_vrf_profile_test
    PRIVATE MDVWB_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

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

add_executable(mdvwb_manager_revision_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_manager_revision_test.cpp
)
target_link_libraries(mdvwb_manager_revision_test PRIVATE mdvwb_manager_mqtt)

add_executable(mdvwb_dashboard_concurrency_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_dashboard_concurrency_test.cpp
)
target_link_libraries(mdvwb_dashboard_concurrency_test PRIVATE mdvwb_manager_mqtt)

add_executable(mdvwb_manager_transaction_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_manager_transaction_test.cpp
)
target_link_libraries(mdvwb_manager_transaction_test PRIVATE mdvwb_manager_mqtt)

add_executable(mdvwb_discovery_runner_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_discovery_runner_test.cpp
)
target_link_libraries(mdvwb_discovery_runner_test PRIVATE mdvwb_discovery_runner)

add_executable(mdvwb_discovery_async_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_discovery_async_test.cpp
)
target_link_libraries(mdvwb_discovery_async_test PRIVATE mdvwb_manager_mqtt)

add_executable(mdvwb_migration_test
    ${MDVWB_MANAGER_TEST_DIR}/mdvwb_migration_test.cpp
)
target_link_libraries(mdvwb_migration_test PRIVATE mdvwb_manager_cli)

add_executable(mdvwb_scheduler_test
    ${MDVWB_SCHEDULER_TEST_DIR}/mdvwb_scheduler_test.cpp
)
target_link_libraries(mdvwb_scheduler_test PRIVATE mdvwb_scheduler)

add_executable(mdvwb_scheduler_freshness_test
    ${MDVWB_SCHEDULER_TEST_DIR}/mdvwb_scheduler_freshness_test.cpp
)
target_link_libraries(mdvwb_scheduler_freshness_test PRIVATE mdvwb_scheduler)

add_executable(mdvwb_mqtt_command_delivery_test
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/mdvwb_mqtt_command_delivery_test.cpp
)
target_link_libraries(mdvwb_mqtt_command_delivery_test
    PRIVATE mdvwb_manager_mqtt mdvwb_scheduler)

set(MDVWB_PROJECT_TARGETS
    MDVWB
    mdvwb_json
    mdvwb_json_test
    mdvwb_mosquitto_transport
    mdvwb_serial_port
    mdvwb_modbus_rtu
    mdvwb_modbus_profile
    mdvwb_modbus_scan
    mdvwb-offline
    mdvwb_mqtt_delivery_test
    mdvwb_driver_fairness_test
    mdvwb_modbus_rtu_test
    mdvwb_modbus_rtu_serial_test
    mdvwb_modbus_profile_test
    mdvwb_modbus_value_test
    mdvwb_modbus_semantic_test
    mdvwb_modbus_resolver_test
    mdvwb_modbus_scan_test
    mdvwb_modbus_scan_execute_test
    mdvwb_modbus_vrf_reference_test
    mdvwb_modbus_vrf_profile_test
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
    mdvwb_manager_revision_test
    mdvwb_dashboard_concurrency_test
    mdvwb_manager_transaction_test
    mdvwb_discovery_runner_test
    mdvwb_discovery_async_test
    mdvwb_migration_test
    mdvwb_scheduler_test
    mdvwb_scheduler_freshness_test
    mdvwb_mqtt_command_delivery_test
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
add_test(NAME mdvwb_json_test COMMAND mdvwb_json_test)
add_test(NAME mdv_protocol_self_test COMMAND MDVWB --self-test)
add_test(NAME mdvwb_offline_publisher_test COMMAND mdvwb-offline --self-test)
add_test(NAME mdvwb_mqtt_delivery_test COMMAND mdvwb_mqtt_delivery_test)
add_test(NAME mdvwb_driver_fairness_test COMMAND mdvwb_driver_fairness_test)
add_test(NAME mdvwb_modbus_rtu_test COMMAND mdvwb_modbus_rtu_test)
add_test(NAME mdvwb_modbus_rtu_serial_test COMMAND mdvwb_modbus_rtu_serial_test)
add_test(NAME mdvwb_modbus_profile_test COMMAND mdvwb_modbus_profile_test)
add_test(NAME mdvwb_modbus_value_test COMMAND mdvwb_modbus_value_test)
add_test(NAME mdvwb_modbus_semantic_test COMMAND mdvwb_modbus_semantic_test)
add_test(NAME mdvwb_modbus_resolver_test COMMAND mdvwb_modbus_resolver_test)
add_test(NAME mdvwb_modbus_scan_test COMMAND mdvwb_modbus_scan_test)
add_test(NAME mdvwb_modbus_scan_execute_test COMMAND mdvwb_modbus_scan_execute_test)
add_test(NAME mdvwb_modbus_vrf_reference_test COMMAND mdvwb_modbus_vrf_reference_test)
add_test(NAME mdvwb_modbus_vrf_profile_test COMMAND mdvwb_modbus_vrf_profile_test)
add_test(NAME mdv_buses_config_test COMMAND mdvwb_buses_config_test)
add_test(NAME mdvwb_dashboard_config_test COMMAND mdvwb_dashboard_config_test)
add_test(NAME mdvwb_schedules_config_test COMMAND mdvwb_schedules_config_test)
add_test(NAME mdvwb_dashboard_upload_test COMMAND mdvwb_dashboard_upload_test)
add_test(NAME mdvwb_manager_cli_test COMMAND mdvwb_manager_cli_test)
add_test(NAME mdvwb_service_sync_test COMMAND mdvwb_service_sync_test)
add_test(NAME mdvwb_manager_mqtt_test COMMAND mdvwb_manager_mqtt_test)
add_test(NAME mdvwb_manager_revision_test COMMAND mdvwb_manager_revision_test)
add_test(NAME mdvwb_dashboard_concurrency_test COMMAND mdvwb_dashboard_concurrency_test)
add_test(NAME mdvwb_manager_transaction_test COMMAND mdvwb_manager_transaction_test)
add_test(NAME mdvwb_discovery_runner_test COMMAND mdvwb_discovery_runner_test)
add_test(NAME mdvwb_discovery_async_test COMMAND mdvwb_discovery_async_test)
set_tests_properties(mdvwb_discovery_async_test PROPERTIES TIMEOUT 10)
add_test(NAME mdvwb_migration_test COMMAND mdvwb_migration_test)
add_test(NAME mdvwb_scheduler_test COMMAND mdvwb_scheduler_test)
add_test(NAME mdvwb_scheduler_freshness_test COMMAND mdvwb_scheduler_freshness_test)
add_test(NAME mdvwb_mqtt_command_delivery_test COMMAND mdvwb_mqtt_command_delivery_test)

include(GNUInstallDirs)
install(TARGETS MDVWB mdvwb-offline mdvwb-manager mdvwb-scheduler
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
