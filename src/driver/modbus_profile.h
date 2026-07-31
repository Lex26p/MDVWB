#pragma once

#include "serial_port.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mdv::modbus {

inline constexpr int kProfileSchemaVersion = 1;
inline constexpr std::uint8_t kMinLogicalAddress = 1;
inline constexpr std::uint8_t kMaxLogicalAddress = 63;

class ProfileError final : public std::runtime_error {
public:
    explicit ProfileError(std::string message);
};

enum class AddressingType {
    DirectSlave,
    FixedSlaveStride,
    Explicit,
};

enum class RegisterSpace {
    Coil,
    DiscreteInput,
    HoldingRegister,
    InputRegister,
};

enum class PointType {
    Boolean,
    Enum,
    Number,
};

enum class WriteRounding {
    Exact,
    Nearest,
    Floor,
    Ceil,
};

enum class RawType {
    UInt16,
    Int16,
};

struct DirectSlaveAddressing {
    std::uint8_t logicalMin = kMinLogicalAddress;
    std::uint8_t logicalMax = kMaxLogicalAddress;
    std::uint16_t registerOffset = 0;
};

struct FixedSlaveStrideAddressing {
    std::uint8_t logicalMin = kMinLogicalAddress;
    std::uint8_t logicalMax = kMaxLogicalAddress;
    std::uint8_t slaveId = 1;
    std::uint8_t firstLogicalAddress = kMinLogicalAddress;
    std::uint16_t registerStride = 0;
};

struct ExplicitDeviceLocation {
    std::uint8_t slaveId = 1;
    std::uint16_t registerOffset = 0;
};

struct ExplicitAddressing {
    std::uint8_t logicalMin = kMinLogicalAddress;
    std::uint8_t logicalMax = kMaxLogicalAddress;
    std::map<std::uint8_t, ExplicitDeviceLocation> devices;
};

using Addressing = std::variant<
    DirectSlaveAddressing,
    FixedSlaveStrideAddressing,
    ExplicitAddressing>;

struct RegisterLocation {
    RegisterSpace space = RegisterSpace::HoldingRegister;
    std::uint16_t address = 0;
    std::optional<std::string> reference;
};

struct NumericTransform {
    double scale = 1.0;
    double offset = 0.0;
};

struct NumericLimits {
    std::optional<double> minimum;
    std::optional<double> maximum;
    std::optional<double> step;
};

struct EnumMappings {
    std::map<std::uint16_t, std::string> read;
    std::map<std::string, std::uint16_t, std::less<>> write;
};

struct PointDefinition {
    PointType type = PointType::Boolean;
    RawType rawType = RawType::UInt16;
    std::optional<RegisterLocation> read;
    std::optional<RegisterLocation> write;
    std::optional<NumericTransform> transform;
    std::optional<NumericLimits> limits;
    WriteRounding rounding = WriteRounding::Exact;
    EnumMappings enumMappings;
};

struct ProfileCapabilities {
    bool power = false;
    bool mode = false;
    bool fanSpeed = false;
    bool setTemperature = false;
    bool roomTemperature = false;
    bool alarm = false;
    bool blinds = false;
    bool blocked = false;
};

enum class ProbePresence {
    AnyResponse,
    AnyNonZero,
};

struct ProbeDefinition {
    RegisterLocation read;
    std::uint16_t quantity = 1;
    ProbePresence presence = ProbePresence::AnyResponse;
};

struct ModbusProfile {
    int schemaVersion = kProfileSchemaVersion;
    std::string id;
    std::string name;
    std::string registerAddressing;
    SerialSettings transport{
        .baudRate = 9600,
        .dataBits = 8,
        .parity = SerialParity::None,
        .stopBits = 1,
    };
    Addressing addressing = DirectSlaveAddressing{};
    ProfileCapabilities capabilities;
    ProbeDefinition probe;
    std::map<std::string, PointDefinition, std::less<>> points;
};

struct ProfileLoadIssue {
    std::filesystem::path path;
    std::string error;
};

struct ProfileCatalog {
    std::map<std::string, ModbusProfile, std::less<>> profiles;
    std::vector<ProfileLoadIssue> issues;

    [[nodiscard]] const ModbusProfile* Find(std::string_view id) const noexcept;
    [[nodiscard]] bool HasErrors() const noexcept;
};

[[nodiscard]] ModbusProfile ParseProfile(std::string_view jsonText);
[[nodiscard]] ModbusProfile LoadProfileFile(const std::filesystem::path& path);

// Loads regular *.json files from one directory, without recursion.
// Invalid files are isolated in issues. Duplicate IDs reject every file in
// that duplicate group so profile selection never depends on directory order.
[[nodiscard]] ProfileCatalog LoadProfileDirectory(
    const std::filesystem::path& directory);

} // namespace mdv::modbus
