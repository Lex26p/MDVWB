#include "modbus_rtu.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace mdv::modbus {
namespace {

void ValidateSlaveId(std::uint8_t slaveId)
{
    if (slaveId < kMinSlaveId || slaveId > kMaxSlaveId) {
        throw std::out_of_range("Modbus slave ID must be in range 1..247");
    }
}

void ValidateRegisterRange(std::uint16_t startAddress, std::uint16_t quantity)
{
    if (quantity == 0) {
        throw std::invalid_argument("Modbus register quantity must be positive");
    }

    const auto lastAddress = static_cast<std::uint32_t>(startAddress) +
        static_cast<std::uint32_t>(quantity) - 1U;
    if (lastAddress > std::numeric_limits<std::uint16_t>::max()) {
        throw std::out_of_range("Modbus register range exceeds 0xFFFF");
    }
}

void AppendU16(RtuAdu& adu, std::uint16_t value)
{
    adu.push_back(static_cast<std::uint8_t>(value >> 8U));
    adu.push_back(static_cast<std::uint8_t>(value & 0xFFU));
}

[[nodiscard]] std::uint16_t ReadU16(
    std::span<const std::uint8_t> bytes,
    std::size_t offset) noexcept
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(bytes[offset]) << 8U) |
        static_cast<std::uint16_t>(bytes[offset + 1]));
}

void AppendCrc(RtuAdu& adu)
{
    const auto crc = CalculateCrc(adu);
    adu.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    adu.push_back(static_cast<std::uint8_t>(crc >> 8U));
}

[[nodiscard]] ParsedResponse InvalidResponse(
    std::uint8_t slaveId,
    Function function,
    std::string error)
{
    ParsedResponse response;
    response.status = ResponseStatus::Invalid;
    response.slaveId = slaveId;
    response.function = function;
    response.error = std::move(error);
    return response;
}

} // namespace

std::uint16_t CalculateCrc(std::span<const std::uint8_t> bytes) noexcept
{
    std::uint16_t crc = 0xFFFFU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 0x0001U) != 0U) {
                crc = static_cast<std::uint16_t>((crc >> 1U) ^ 0xA001U);
            }
            else {
                crc = static_cast<std::uint16_t>(crc >> 1U);
            }
        }
    }
    return crc;
}

bool HasValidCrc(std::span<const std::uint8_t> adu) noexcept
{
    if (adu.size() < 4) {
        return false;
    }
    const auto payload = adu.first(adu.size() - 2);
    const auto expected = CalculateCrc(payload);
    const auto received = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(adu[adu.size() - 2]) |
        (static_cast<std::uint16_t>(adu[adu.size() - 1]) << 8U));
    return expected == received;
}

RtuAdu BuildReadHoldingRegistersRequest(
    std::uint8_t slaveId,
    std::uint16_t startAddress,
    std::uint16_t quantity)
{
    ValidateSlaveId(slaveId);
    ValidateRegisterRange(startAddress, quantity);
    if (quantity > kMaxReadRegisters) {
        throw std::out_of_range("Modbus FC03 quantity must be in range 1..125");
    }

    RtuAdu adu;
    adu.reserve(8);
    adu.push_back(slaveId);
    adu.push_back(static_cast<std::uint8_t>(Function::ReadHoldingRegisters));
    AppendU16(adu, startAddress);
    AppendU16(adu, quantity);
    AppendCrc(adu);
    return adu;
}

RtuAdu BuildWriteMultipleRegistersRequest(
    std::uint8_t slaveId,
    std::uint16_t startAddress,
    std::span<const std::uint16_t> values)
{
    ValidateSlaveId(slaveId);
    if (values.empty()) {
        throw std::invalid_argument("Modbus FC10 requires at least one register");
    }
    if (values.size() > kMaxWriteRegisters) {
        throw std::out_of_range("Modbus FC10 quantity must be in range 1..123");
    }

    const auto quantity = static_cast<std::uint16_t>(values.size());
    ValidateRegisterRange(startAddress, quantity);

    RtuAdu adu;
    adu.reserve(9U + values.size() * 2U);
    adu.push_back(slaveId);
    adu.push_back(static_cast<std::uint8_t>(Function::WriteMultipleRegisters));
    AppendU16(adu, startAddress);
    AppendU16(adu, quantity);
    adu.push_back(static_cast<std::uint8_t>(values.size() * 2U));
    for (const auto value : values) {
        AppendU16(adu, value);
    }
    AppendCrc(adu);
    return adu;
}

ParsedResponse ParseResponse(
    std::span<const std::uint8_t> adu,
    std::uint8_t expectedSlaveId,
    Function expectedFunction)
{
    ValidateSlaveId(expectedSlaveId);

    if (adu.size() < 5) {
        return InvalidResponse(
            expectedSlaveId, expectedFunction, "Modbus response is too short");
    }
    if (adu.size() > kMaxRtuAduSize) {
        return InvalidResponse(
            expectedSlaveId, expectedFunction, "Modbus response exceeds 256 bytes");
    }
    if (!HasValidCrc(adu)) {
        return InvalidResponse(
            expectedSlaveId, expectedFunction, "Modbus response CRC is invalid");
    }
    if (adu[0] != expectedSlaveId) {
        return InvalidResponse(
            adu[0], expectedFunction, "Modbus response slave ID does not match request");
    }

    const auto expectedFunctionByte = static_cast<std::uint8_t>(expectedFunction);
    const auto functionByte = adu[1];
    if (functionByte == static_cast<std::uint8_t>(expectedFunctionByte | 0x80U)) {
        if (adu.size() != 5) {
            return InvalidResponse(
                expectedSlaveId, expectedFunction,
                "Modbus exception response must contain exactly 5 bytes");
        }

        ParsedResponse response;
        response.status = ResponseStatus::Exception;
        response.slaveId = expectedSlaveId;
        response.function = expectedFunction;
        response.exceptionCode = adu[2];
        return response;
    }

    if (functionByte != expectedFunctionByte) {
        return InvalidResponse(
            expectedSlaveId, expectedFunction,
            "Modbus response function does not match request");
    }

    if (expectedFunction == Function::ReadHoldingRegisters) {
        const auto byteCount = static_cast<std::size_t>(adu[2]);
        if (byteCount == 0 || (byteCount % 2U) != 0U) {
            return InvalidResponse(
                expectedSlaveId, expectedFunction,
                "Modbus FC03 response byte count must be positive and even");
        }
        if (adu.size() != byteCount + 5U) {
            return InvalidResponse(
                expectedSlaveId, expectedFunction,
                "Modbus FC03 response length does not match byte count");
        }

        ParsedResponse response;
        response.status = ResponseStatus::Success;
        response.slaveId = expectedSlaveId;
        response.function = expectedFunction;
        response.registers.reserve(byteCount / 2U);
        for (std::size_t offset = 3; offset < 3U + byteCount; offset += 2U) {
            response.registers.push_back(ReadU16(adu, offset));
        }
        return response;
    }

    if (expectedFunction == Function::WriteMultipleRegisters) {
        if (adu.size() != 8) {
            return InvalidResponse(
                expectedSlaveId, expectedFunction,
                "Modbus FC10 response must contain exactly 8 bytes");
        }

        ParsedResponse response;
        response.status = ResponseStatus::Success;
        response.slaveId = expectedSlaveId;
        response.function = expectedFunction;
        response.startAddress = ReadU16(adu, 2);
        response.quantity = ReadU16(adu, 4);
        if (*response.quantity == 0 || *response.quantity > kMaxWriteRegisters) {
            return InvalidResponse(
                expectedSlaveId, expectedFunction,
                "Modbus FC10 response quantity is outside 1..123");
        }
        return response;
    }

    return InvalidResponse(
        expectedSlaveId, expectedFunction, "unsupported Modbus function");
}

std::optional<RtuAdu> ResponseCollector::Push(std::uint8_t byte)
{
    if (buffer_.empty()) {
        if (byte < kMinSlaveId || byte > kMaxSlaveId) {
            return std::nullopt;
        }
        buffer_.push_back(byte);
        return std::nullopt;
    }

    if (buffer_.size() >= kMaxRtuAduSize) {
        buffer_.clear();
        return std::nullopt;
    }

    buffer_.push_back(byte);

    if (buffer_.size() == 2) {
        const auto function = buffer_[1];
        const auto normal =
            function == static_cast<std::uint8_t>(Function::ReadHoldingRegisters) ||
            function == static_cast<std::uint8_t>(Function::WriteMultipleRegisters);
        const auto exception =
            function == (static_cast<std::uint8_t>(Function::ReadHoldingRegisters) | 0x80U) ||
            function == (static_cast<std::uint8_t>(Function::WriteMultipleRegisters) | 0x80U);
        if (!normal && !exception) {
            buffer_.clear();
            return std::nullopt;
        }
    }

    const auto expectedSize = ExpectedSize();
    if (!expectedSize.has_value() || buffer_.size() < *expectedSize) {
        return std::nullopt;
    }

    if (buffer_.size() == *expectedSize) {
        auto result = std::move(buffer_);
        buffer_.clear();
        return result;
    }

    buffer_.clear();
    return std::nullopt;
}

void ResponseCollector::Reset() noexcept
{
    buffer_.clear();
}


std::optional<std::size_t> ResponseCollector::ExpectedSize() const noexcept
{
    if (buffer_.size() < 2) {
        return std::nullopt;
    }

    const auto function = buffer_[1];
    if ((function & 0x80U) != 0U) {
        return 5U;
    }
    if (function == static_cast<std::uint8_t>(Function::WriteMultipleRegisters)) {
        return 8U;
    }
    if (function == static_cast<std::uint8_t>(Function::ReadHoldingRegisters)) {
        if (buffer_.size() < 3) {
            return std::nullopt;
        }
        return static_cast<std::size_t>(buffer_[2]) + 5U;
    }
    return std::nullopt;
}

} // namespace mdv::modbus
