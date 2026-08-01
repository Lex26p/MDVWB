#pragma once

#include "modbus_rtu_serial.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mdvwb {

enum class DiscoveryRuntimeProtocol {
    Mdv,
    ModbusRtu,
};

struct ModbusDiscoveryRuntime {
    std::string port;
    std::filesystem::path profileDirectory;
    std::string profileId;
    mdv::SerialSettings serial{
        .baudRate = 9600,
        .dataBits = 8,
        .parity = mdv::SerialParity::None,
        .stopBits = 1,
    };
    std::chrono::milliseconds responseTimeout{200};
};

struct DiscoveryRuntimeSelection {
    DiscoveryRuntimeProtocol protocol = DiscoveryRuntimeProtocol::Mdv;
    std::optional<ModbusDiscoveryRuntime> modbus;
};

struct ModbusDiscoveryScanResult {
    bool success = false;
    std::vector<int> addresses;
    std::string output;
    std::string message;
};

// Looks only at manager-generated /etc/default-style files. No matching file
// means that the legacy MDV discovery process should be used unchanged.
[[nodiscard]] std::optional<DiscoveryRuntimeSelection>
FindDiscoveryRuntimeForPort(
    const std::filesystem::path& defaultDirectory,
    std::string_view port);

[[nodiscard]] std::filesystem::path DiscoveryDefaultDirectoryFromEnvironment();

// Executes the selected profile's safe read-only probe for logical addresses
// 1..63. Unsupported candidates generate no traffic. Any transport/protocol
// error rejects the whole result instead of returning a misleading partial set.
[[nodiscard]] ModbusDiscoveryScanResult ExecuteModbusDiscovery(
    const ModbusDiscoveryRuntime& runtime,
    mdv::modbus::ITransactionTransport& transport);

// Real serial entry point used by NativeDiscoveryRunner after the bus service
// has been stopped by ManagerMqttService.
[[nodiscard]] ModbusDiscoveryScanResult RunModbusDiscovery(
    const ModbusDiscoveryRuntime& runtime);

} // namespace mdvwb
