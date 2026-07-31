#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mdv::modbus {

inline constexpr std::uint8_t kMinSlaveId = 1;
inline constexpr std::uint8_t kMaxSlaveId = 247;
inline constexpr std::size_t kMaxRtuAduSize = 256;
inline constexpr std::uint16_t kMaxReadRegisters = 125;
inline constexpr std::uint16_t kMaxWriteRegisters = 123;

enum class Function : std::uint8_t {
    ReadHoldingRegisters = 0x03,
    WriteMultipleRegisters = 0x10,
};

enum class ResponseStatus {
    Success,
    Exception,
    Invalid,
};

using RtuAdu = std::vector<std::uint8_t>;

struct ParsedResponse {
    ResponseStatus status = ResponseStatus::Invalid;
    std::uint8_t slaveId = 0;
    Function function = Function::ReadHoldingRegisters;
    std::vector<std::uint16_t> registers;
    std::optional<std::uint16_t> startAddress;
    std::optional<std::uint16_t> quantity;
    std::optional<std::uint8_t> exceptionCode;
    std::string error;
};

[[nodiscard]] std::uint16_t CalculateCrc(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] bool HasValidCrc(std::span<const std::uint8_t> adu) noexcept;

[[nodiscard]] RtuAdu BuildReadHoldingRegistersRequest(
    std::uint8_t slaveId,
    std::uint16_t startAddress,
    std::uint16_t quantity);

[[nodiscard]] RtuAdu BuildWriteMultipleRegistersRequest(
    std::uint8_t slaveId,
    std::uint16_t startAddress,
    std::span<const std::uint16_t> values);

[[nodiscard]] ParsedResponse ParseResponse(
    std::span<const std::uint8_t> adu,
    std::uint8_t expectedSlaveId,
    Function expectedFunction);

// Collects supported variable-length Modbus RTU responses from a byte stream.
// CRC and semantic validation are intentionally performed by ParseResponse().
class ResponseCollector {
public:
    [[nodiscard]] std::optional<RtuAdu> Push(std::uint8_t byte);
    void Reset() noexcept;

private:
    [[nodiscard]] std::optional<std::size_t> ExpectedSize() const noexcept;

    RtuAdu buffer_;
};

} // namespace mdv::modbus
