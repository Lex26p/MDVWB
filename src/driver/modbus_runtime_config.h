#pragma once

#include "modbus_profile.h"
#include "serial_port.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mdv::modbus {

struct RuntimeMqttSettings {
    std::string host = "127.0.0.1";
    int port = 1883;
    int keepAliveSeconds = 60;
    std::string clientId;
    std::string username;
    std::string password;
    unsigned int reconnectDelaySeconds = 1;
    unsigned int reconnectDelayMaxSeconds = 10;
};

struct ModbusRuntimeConfig {
    std::vector<std::uint8_t> addresses;
    std::string serialPort;
    int busNumber = 1;

    std::filesystem::path profileDirectory;
    std::string profileId;
    SerialSettings serial{
        .baudRate = 9600,
        .dataBits = 8,
        .parity = SerialParity::None,
        .stopBits = 1,
    };

    std::chrono::milliseconds transactionPeriod{150};
    std::chrono::milliseconds responseTimeout{200};
    RuntimeMqttSettings mqtt;
    bool publishPollAddress = false;
};

using EnvironmentLookup = std::function<
    std::optional<std::string>(std::string_view name)>;

// Parses the managed per-bus environment emitted by mdvwb-manager/mdvwb-run.
// The lookup indirection keeps parsing deterministic and directly testable.
[[nodiscard]] ModbusRuntimeConfig ParseModbusRuntimeConfig(
    const EnvironmentLookup& lookup);

// Loads and revalidates the selected production profile immediately before the
// serial port is opened. Unrelated invalid profile files remain isolated.
[[nodiscard]] ModbusProfile LoadModbusRuntimeProfile(
    const ModbusRuntimeConfig& config);

} // namespace mdv::modbus
