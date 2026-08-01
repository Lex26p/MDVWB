#include "modbus_runtime_config.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef MDVWB_SOURCE_DIR
#error MDVWB_SOURCE_DIR must point to the repository source directory
#endif

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void ExpectInvalid(Function&& function, std::string_view expected)
{
    try {
        function();
    }
    catch (const std::invalid_argument& error) {
        Require(
            std::string_view(error.what()).find(expected) !=
                std::string_view::npos,
            "configuration failed for an unexpected reason");
        return;
    }
    throw std::runtime_error("invalid Modbus runtime configuration was accepted");
}

using Environment = std::map<std::string, std::string, std::less<>>;

mdv::modbus::EnvironmentLookup Lookup(const Environment& environment)
{
    return [&environment](std::string_view name)
        -> std::optional<std::string> {
        const auto iterator = environment.find(name);
        if (iterator == environment.end()) {
            return std::nullopt;
        }
        return iterator->second;
    };
}

Environment ValidEnvironment()
{
    return Environment{
        {"MDVWB_ADDRESSES", "63,1,2"},
        {"MDVWB_PORT", "/dev/ttyRS485-2"},
        {"MDVWB_BUS", "2"},
        {"MDVWB_MODBUS_PROFILE_DIR",
         (std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus").string()},
        {"MDVWB_MODBUS_PROFILE", "vrf_add_controller"},
        {"MDVWB_MODBUS_BAUD_RATE", "9600"},
        {"MDVWB_MODBUS_DATA_BITS", "8"},
        {"MDVWB_MODBUS_PARITY", "none"},
        {"MDVWB_MODBUS_STOP_BITS", "1"},
        {"MDVWB_MODBUS_RESPONSE_TIMEOUT_MS", "230"},
        {"MDVWB_PERIOD_MS", "175"},
        {"MDVWB_MQTT_HOST", "192.0.2.20"},
        {"MDVWB_MQTT_PORT", "1884"},
        {"MDVWB_MQTT_USER", "operator"},
        {"MDVWB_MQTT_PASSWORD", "secret"},
        {"MDVWB_MQTT_KEEPALIVE", "45"},
        {"MDVWB_MQTT_RECONNECT", "2"},
        {"MDVWB_MQTT_RECONNECT_MAX", "20"},
        {"MDVWB_PUBLISH_POLL_ADDRESS", "1"},
    };
}

void TestValidManagedEnvironment()
{
    const auto environment = ValidEnvironment();
    const auto config = mdv::modbus::ParseModbusRuntimeConfig(
        Lookup(environment));

    Require(
        config.addresses == std::vector<std::uint8_t>({1U, 2U, 63U}),
        "logical addresses were not normalized");
    Require(config.serialPort == "/dev/ttyRS485-2", "serial port mismatch");
    Require(config.busNumber == 2, "bus number mismatch");
    Require(config.profileId == "vrf_add_controller", "profile ID mismatch");
    Require(config.serial.baudRate == 9600U, "baud rate mismatch");
    Require(config.serial.dataBits == 8U, "data bits mismatch");
    Require(
        config.serial.parity == mdv::SerialParity::None,
        "parity mismatch");
    Require(config.serial.stopBits == 1U, "stop bits mismatch");
    Require(
        config.responseTimeout == std::chrono::milliseconds(230),
        "response timeout mismatch");
    Require(
        config.transactionPeriod == std::chrono::milliseconds(175),
        "transaction period mismatch");
    Require(config.mqtt.host == "192.0.2.20", "MQTT host mismatch");
    Require(config.mqtt.port == 1884, "MQTT port mismatch");
    Require(config.mqtt.username == "operator", "MQTT username mismatch");
    Require(config.mqtt.password == "secret", "MQTT password mismatch");
    Require(config.mqtt.clientId == "mdvwb-2", "MQTT client ID mismatch");
    Require(config.publishPollAddress, "poll-address flag mismatch");

    const auto profile = mdv::modbus::LoadModbusRuntimeProfile(config);
    Require(
        profile.id == "vrf_add_controller",
        "runtime loaded the wrong profile");
}

void TestDefaultsRemainDeterministic()
{
    Environment environment{
        {"MDVWB_ADDRESSES", "1"},
        {"MDVWB_PORT", "/dev/ttyUSB0"},
        {"MDVWB_BUS", "1"},
        {"MDVWB_MODBUS_PROFILE_DIR",
         (std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus").string()},
        {"MDVWB_MODBUS_PROFILE", "vrf_add_controller"},
    };

    const auto config = mdv::modbus::ParseModbusRuntimeConfig(
        Lookup(environment));
    Require(config.serial.baudRate == 9600U, "default baud rate mismatch");
    Require(config.responseTimeout == std::chrono::milliseconds(200),
            "default response timeout mismatch");
    Require(config.transactionPeriod == std::chrono::milliseconds(150),
            "default transaction period mismatch");
    Require(config.mqtt.host == "127.0.0.1", "default MQTT host mismatch");
    Require(config.mqtt.port == 1883, "default MQTT port mismatch");
    Require(!config.publishPollAddress, "default poll-address flag mismatch");
}

void TestInvalidEnvironmentRejected()
{
    auto environment = ValidEnvironment();
    environment["MDVWB_ADDRESSES"] = "0,1";
    ExpectInvalid(
        [&] {
            static_cast<void>(mdv::modbus::ParseModbusRuntimeConfig(
                Lookup(environment)));
        },
        "range 1..63");

    environment = ValidEnvironment();
    environment["MDVWB_ADDRESSES"] = "1,1";
    ExpectInvalid(
        [&] {
            static_cast<void>(mdv::modbus::ParseModbusRuntimeConfig(
                Lookup(environment)));
        },
        "duplicate");

    environment = ValidEnvironment();
    environment["MDVWB_MODBUS_PARITY"] = "mark";
    ExpectInvalid(
        [&] {
            static_cast<void>(mdv::modbus::ParseModbusRuntimeConfig(
                Lookup(environment)));
        },
        "none, even or odd");

    environment = ValidEnvironment();
    environment.erase("MDVWB_MODBUS_PROFILE");
    ExpectInvalid(
        [&] {
            static_cast<void>(mdv::modbus::ParseModbusRuntimeConfig(
                Lookup(environment)));
        },
        "MDVWB_MODBUS_PROFILE");
}

void TestProfileSelectionAndTransportRevalidated()
{
    auto environment = ValidEnvironment();
    environment["MDVWB_MODBUS_PROFILE"] = "missing_profile";
    const auto missing = mdv::modbus::ParseModbusRuntimeConfig(
        Lookup(environment));
    ExpectInvalid(
        [&] {
            static_cast<void>(mdv::modbus::LoadModbusRuntimeProfile(missing));
        },
        "not available");

    environment = ValidEnvironment();
    environment["MDVWB_MODBUS_BAUD_RATE"] = "19200";
    const auto mismatch = mdv::modbus::ParseModbusRuntimeConfig(
        Lookup(environment));
    ExpectInvalid(
        [&] {
            static_cast<void>(mdv::modbus::LoadModbusRuntimeProfile(mismatch));
        },
        "do not match");
}

} // namespace

int main()
{
    try {
        TestValidManagedEnvironment();
        TestDefaultsRemainDeterministic();
        TestInvalidEnvironmentRejected();
        TestProfileSelectionAndTransportRevalidated();
        std::cout << "MDVWB Modbus runtime configuration tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus runtime configuration tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
