#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mdvwb {

enum class BusProtocol {
    Mdv,
    ModbusRtu,
};

enum class BusParity {
    None,
    Even,
    Odd,
};

struct ModbusBusSettings {
    std::string profileId;
    int baudRate = 9600;
    int dataBits = 8;
    BusParity parity = BusParity::None;
    int stopBits = 1;
};

struct BusConfig {
    int id = 0;
    bool enabled = false;
    std::string port;
    std::vector<int> addresses;

    // Backward compatibility: an old buses.json without "protocol" remains MDV.
    BusProtocol protocol = BusProtocol::Mdv;
    std::optional<ModbusBusSettings> modbus;
};

struct BusesConfig {
    int version = 1;
    std::vector<BusConfig> buses;
    int revision = 0;
};

class BusesConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[nodiscard]] std::string_view BusProtocolName(BusProtocol protocol) noexcept;
[[nodiscard]] std::string_view BusParityName(BusParity parity) noexcept;

BusesConfig ParseBusesConfig(std::string_view jsonText);
BusesConfig LoadBusesConfig(const std::filesystem::path& path);
std::string SerializeBusesConfig(const BusesConfig& config);

}  // namespace mdvwb
