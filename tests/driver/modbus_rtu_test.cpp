#include "modbus_rtu.h"

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

bool Check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

template <typename Left, typename Right>
bool CheckEqual(const Left& left, const Right& right, std::string_view message)
{
    return Check(left == right, message);
}

bool TestCrcVector()
{
    constexpr std::array<std::uint8_t, 6> payload{
        0x01, 0x03, 0x00, 0x6B, 0x00, 0x03};
    return Check(
        mdv::modbus::CalculateCrc(payload) == 0x1774,
        "CRC16 fixture 01 03 006B 0003");
}

bool TestReadRequest()
{
    const std::vector<std::uint8_t> expected{
        0x01, 0x03, 0x00, 0x6B, 0x00, 0x03, 0x74, 0x17};
    const auto actual = mdv::modbus::BuildReadHoldingRegistersRequest(
        1, 0x006B, 3);
    return CheckEqual(actual, expected, "FC03 request fixture") &&
        Check(mdv::modbus::HasValidCrc(actual), "FC03 request CRC");
}

bool TestWriteRequest()
{
    constexpr std::array<std::uint16_t, 2> values{0x000A, 0x0102};
    const std::vector<std::uint8_t> expected{
        0x01, 0x10, 0x00, 0x01, 0x00, 0x02,
        0x04, 0x00, 0x0A, 0x01, 0x02, 0x92, 0x30};
    const auto actual = mdv::modbus::BuildWriteMultipleRegistersRequest(
        1, 0x0001, values);
    return CheckEqual(actual, expected, "FC10 request fixture") &&
        Check(mdv::modbus::HasValidCrc(actual), "FC10 request CRC");
}

bool TestReadResponse()
{
    const std::vector<std::uint8_t> response{
        0x01, 0x03, 0x06, 0x02, 0x2B, 0x00, 0x00, 0x00, 0x64, 0x05, 0x7A};
    const auto parsed = mdv::modbus::ParseResponse(
        response, 1, mdv::modbus::Function::ReadHoldingRegisters);

    return Check(parsed.status == mdv::modbus::ResponseStatus::Success,
                 "FC03 response status") &&
        Check(parsed.registers.size() == 3, "FC03 register count") &&
        Check(parsed.registers[0] == 0x022B, "FC03 register 0") &&
        Check(parsed.registers[1] == 0x0000, "FC03 register 1") &&
        Check(parsed.registers[2] == 0x0064, "FC03 register 2");
}

bool TestWriteResponse()
{
    const std::vector<std::uint8_t> response{
        0x01, 0x10, 0x00, 0x01, 0x00, 0x02, 0x10, 0x08};
    const auto parsed = mdv::modbus::ParseResponse(
        response, 1, mdv::modbus::Function::WriteMultipleRegisters);

    return Check(parsed.status == mdv::modbus::ResponseStatus::Success,
                 "FC10 response status") &&
        Check(parsed.startAddress == 0x0001, "FC10 echoed start address") &&
        Check(parsed.quantity == 2, "FC10 echoed quantity");
}

bool TestExceptionResponse()
{
    const std::vector<std::uint8_t> response{
        0x01, 0x83, 0x02, 0xC0, 0xF1};
    const auto parsed = mdv::modbus::ParseResponse(
        response, 1, mdv::modbus::Function::ReadHoldingRegisters);

    return Check(parsed.status == mdv::modbus::ResponseStatus::Exception,
                 "Modbus exception status") &&
        Check(parsed.exceptionCode == 0x02, "Modbus exception code");
}

bool TestInvalidResponses()
{
    auto badCrc = std::vector<std::uint8_t>{
        0x01, 0x03, 0x02, 0x00, 0x01, 0x79, 0x84};
    badCrc.back() ^= 0x01;

    const auto crc = mdv::modbus::ParseResponse(
        badCrc, 1, mdv::modbus::Function::ReadHoldingRegisters);

    const std::vector<std::uint8_t> wrongSlave{
        0x02, 0x10, 0x00, 0x01, 0x00, 0x02, 0x10, 0x3B};
    const auto slave = mdv::modbus::ParseResponse(
        wrongSlave, 1, mdv::modbus::Function::WriteMultipleRegisters);

    const std::vector<std::uint8_t> oddByteCount{
        0x01, 0x03, 0x01, 0x00, 0xF0, 0x48};
    const auto odd = mdv::modbus::ParseResponse(
        oddByteCount, 1, mdv::modbus::Function::ReadHoldingRegisters);

    return Check(crc.status == mdv::modbus::ResponseStatus::Invalid,
                 "bad CRC rejected") &&
        Check(slave.status == mdv::modbus::ResponseStatus::Invalid,
              "wrong slave rejected") &&
        Check(odd.status == mdv::modbus::ResponseStatus::Invalid,
              "odd FC03 byte count rejected");
}

bool TestRequestValidation()
{
    bool slaveRejected = false;
    try {
        static_cast<void>(mdv::modbus::BuildReadHoldingRegistersRequest(0, 0, 1));
    }
    catch (const std::out_of_range&) {
        slaveRejected = true;
    }

    bool zeroRejected = false;
    try {
        static_cast<void>(mdv::modbus::BuildReadHoldingRegistersRequest(1, 0, 0));
    }
    catch (const std::invalid_argument&) {
        zeroRejected = true;
    }

    bool overflowRejected = false;
    try {
        static_cast<void>(
            mdv::modbus::BuildReadHoldingRegistersRequest(1, 0xFFFF, 2));
    }
    catch (const std::out_of_range&) {
        overflowRejected = true;
    }

    bool tooManyWriteRejected = false;
    try {
        std::vector<std::uint16_t> values(124, 0);
        static_cast<void>(
            mdv::modbus::BuildWriteMultipleRegistersRequest(1, 0, values));
    }
    catch (const std::out_of_range&) {
        tooManyWriteRejected = true;
    }

    return Check(slaveRejected, "slave 0 rejected") &&
        Check(zeroRejected, "zero register count rejected") &&
        Check(overflowRejected, "register range overflow rejected") &&
        Check(tooManyWriteRejected, "FC10 >123 registers rejected");
}

bool TestResponseCollector()
{
    const std::vector<std::uint8_t> readResponse{
        0x01, 0x03, 0x06, 0x02, 0x2B, 0x00, 0x00, 0x00, 0x64, 0x05, 0x7A};
    const std::vector<std::uint8_t> exceptionResponse{
        0x01, 0x83, 0x02, 0xC0, 0xF1};

    mdv::modbus::ResponseCollector collector;
    std::optional<mdv::modbus::RtuAdu> first;
    std::optional<mdv::modbus::RtuAdu> second;

    for (const auto byte : std::vector<std::uint8_t>{0x00, 0xFF, 0x00}) {
        static_cast<void>(collector.Push(byte));
    }
    for (const auto byte : readResponse) {
        if (auto frame = collector.Push(byte); frame.has_value()) {
            first = std::move(frame);
        }
    }
    for (const auto byte : exceptionResponse) {
        if (auto frame = collector.Push(byte); frame.has_value()) {
            second = std::move(frame);
        }
    }

    return Check(first.has_value(), "collector extracted FC03 response") &&
        CheckEqual(*first, readResponse, "collector FC03 bytes") &&
        Check(second.has_value(), "collector extracted exception response") &&
        CheckEqual(*second, exceptionResponse, "collector exception bytes");
}

} // namespace

int main()
{
    bool ok = true;
    ok = TestCrcVector() && ok;
    ok = TestReadRequest() && ok;
    ok = TestWriteRequest() && ok;
    ok = TestReadResponse() && ok;
    ok = TestWriteResponse() && ok;
    ok = TestExceptionResponse() && ok;
    ok = TestInvalidResponses() && ok;
    ok = TestRequestValidation() && ok;
    ok = TestResponseCollector() && ok;

    if (!ok) {
        return 1;
    }

    std::cout << "Modbus RTU codec tests passed\n";
    return 0;
}
