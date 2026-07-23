cmake_minimum_required(VERSION 3.18)

project(MDVWB VERSION 1.2.0 LANGUAGES CXX)

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
    mdv_discovery.cpp
    mdv_discovery.h
    mdv_mqtt.cpp
    mdv_mqtt.h
    mdv_metadata.cpp
    mdv_metadata.h
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

add_library(mdvwb_mosquitto_transport STATIC
    mdv_mosquitto.cpp
    mdv_mosquitto.h
)
target_compile_features(mdvwb_mosquitto_transport PUBLIC cxx_std_20)
target_include_directories(mdvwb_mosquitto_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mdvwb_mosquitto_transport PUBLIC Threads::Threads)
if(MOSQUITTO_INCLUDE_DIR AND MOSQUITTO_LIBRARY)
    target_compile_definitions(mdvwb_mosquitto_transport PRIVATE MDVWB_HAS_MOSQUITTO=1)
    target_include_directories(mdvwb_mosquitto_transport PRIVATE ${MOSQUITTO_INCLUDE_DIR})
    target_link_libraries(mdvwb_mosquitto_transport PRIVATE ${MOSQUITTO_LIBRARY})
endif()

if(MSVC)
    target_compile_options(MDVWB PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(MDVWB PRIVATE -Wall -Wextra -Wpedantic)
endif()


add_library(mdvwb_buses_config STATIC
    mdv_buses_config.cpp
    mdv_buses_config.h
)
target_compile_features(mdvwb_buses_config PUBLIC cxx_std_20)
target_include_directories(mdvwb_buses_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(mdvwb_buses_config_test
    tests/mdv_buses_config_test.cpp
)
target_link_libraries(mdvwb_buses_config_test PRIVATE mdvwb_buses_config)

if(MSVC)
    target_compile_options(mdvwb_buses_config PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb_buses_config_test PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(mdvwb_buses_config PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb_buses_config_test PRIVATE -Wall -Wextra -Wpedantic)
endif()


add_library(mdvwb_service_sync STATIC
    mdvwb_service_sync.cpp
    mdvwb_service_sync.h
)
target_compile_features(mdvwb_service_sync PUBLIC cxx_std_20)
target_include_directories(mdvwb_service_sync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mdvwb_service_sync PUBLIC mdvwb_buses_config)

add_library(mdvwb_discovery_runner STATIC
    mdvwb_discovery_runner.cpp
    mdvwb_discovery_runner.h
)
target_compile_features(mdvwb_discovery_runner PUBLIC cxx_std_20)
target_include_directories(mdvwb_discovery_runner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(mdvwb_manager_mqtt STATIC
    mdvwb_manager_mqtt.cpp
    mdvwb_manager_mqtt.h
)
target_compile_features(mdvwb_manager_mqtt PUBLIC cxx_std_20)
target_include_directories(mdvwb_manager_mqtt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mdvwb_manager_mqtt
    PUBLIC mdvwb_buses_config mdvwb_service_sync mdvwb_mosquitto_transport mdvwb_discovery_runner)

add_library(mdvwb_manager_cli STATIC
    mdvwb_manager_cli.cpp
    mdvwb_manager_cli.h
    mdvwb_migration.cpp
    mdvwb_migration.h
)
target_compile_features(mdvwb_manager_cli PUBLIC cxx_std_20)
target_include_directories(mdvwb_manager_cli PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mdvwb_manager_cli PUBLIC mdvwb_buses_config mdvwb_service_sync mdvwb_manager_mqtt)

add_executable(mdvwb-manager
    mdvwb_manager_main.cpp
)
target_link_libraries(mdvwb-manager PRIVATE mdvwb_manager_cli)

add_executable(mdvwb_manager_cli_test
    tests/mdvwb_manager_cli_test.cpp
)
target_link_libraries(mdvwb_manager_cli_test PRIVATE mdvwb_manager_cli)

add_executable(mdvwb_service_sync_test
    tests/mdvwb_service_sync_test.cpp
)
target_link_libraries(mdvwb_service_sync_test PRIVATE mdvwb_service_sync)

add_executable(mdvwb_manager_mqtt_test
    tests/mdvwb_manager_mqtt_test.cpp
)
target_link_libraries(mdvwb_manager_mqtt_test PRIVATE mdvwb_manager_mqtt)

add_executable(mdvwb_discovery_runner_test
    tests/mdvwb_discovery_runner_test.cpp
)
target_link_libraries(mdvwb_discovery_runner_test PRIVATE mdvwb_discovery_runner)

add_executable(mdvwb_migration_test
    tests/mdvwb_migration_test.cpp
)
target_link_libraries(mdvwb_migration_test PRIVATE mdvwb_manager_cli)

if(MSVC)
    target_compile_options(mdvwb_service_sync PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb_service_sync_test PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb_mosquitto_transport PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb_discovery_runner PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb_discovery_runner_test PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb_manager_mqtt PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb_manager_mqtt_test PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb_manager_cli PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb-manager PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb_manager_cli_test PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(mdvwb_migration_test PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(mdvwb_service_sync PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb_service_sync_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb_mosquitto_transport PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb_discovery_runner PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb_discovery_runner_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb_manager_mqtt PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb_manager_mqtt_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb_manager_cli PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb-manager PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb_manager_cli_test PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(mdvwb_migration_test PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME mdv_protocol_self_test COMMAND MDVWB --self-test)
add_test(NAME mdv_buses_config_test COMMAND mdvwb_buses_config_test)
add_test(NAME mdvwb_manager_cli_test COMMAND mdvwb_manager_cli_test)
add_test(NAME mdvwb_service_sync_test COMMAND mdvwb_service_sync_test)
add_test(NAME mdvwb_manager_mqtt_test COMMAND mdvwb_manager_mqtt_test)
add_test(NAME mdvwb_discovery_runner_test COMMAND mdvwb_discovery_runner_test)
add_test(NAME mdvwb_migration_test COMMAND mdvwb_migration_test)

include(GNUInstallDirs)
install(TARGETS MDVWB mdvwb-manager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
