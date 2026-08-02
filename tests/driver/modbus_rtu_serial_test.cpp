#include "modbus_rtu_serial.h"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

bool Check(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        return false;
    }
    return true;
}

bool TestInterFrameDelay()
{
    const auto at9600 = mdv::modbus::CalculateInterFrameDelay({
        .baudRate = 9600,
        .dataBits = 8,
        .parity = mdv::SerialParity::None,
        .stopBits = 1,
    });
    const auto at4800 = mdv::modbus::CalculateInterFrameDelay({
        .baudRate = 4800,
        .dataBits = 8,
        .parity = mdv::SerialParity::None,
        .stopBits = 1,
    });
    const auto at38400 = mdv::modbus::CalculateInterFrameDelay({
        .baudRate = 38400,
        .dataBits = 8,
        .parity = mdv::SerialParity::None,
        .stopBits = 1,
    });

    return Check(
               at9600 == std::chrono::microseconds(3646),
               "9600 8N1 t3.5") &&
        Check(
               at4800 == std::chrono::microseconds(7292),
               "4800 8N1 t3.5") &&
        Check(
               at38400 == std::chrono::microseconds(1750),
               "high-baud fixed t3.5");
}

bool TestResponseDeadlineStartsAfterWriteCompletion()
{
    using Clock = std::chrono::steady_clock;

    const Clock::time_point transactionStarted{
        std::chrono::milliseconds(100)};
    const Clock::time_point writeCompleted =
        transactionStarted + std::chrono::milliseconds(2500);
    const mdv::modbus::RtuTimingSettings timing{
        .responseTimeout = std::chrono::milliseconds(200),
    };

    const auto deadline =
        mdv::modbus::CalculateResponseDeadline(writeCompleted, timing);

    return Check(
               deadline ==
                   writeCompleted + std::chrono::milliseconds(200),
               "response timeout starts at write completion") &&
        Check(
               deadline >
                   transactionStarted + std::chrono::milliseconds(200),
               "request transmission time is not deducted from response timeout");
}

bool TestSerialValidation()
{
    bool badBaudRejected = false;
    try {
        mdv::ValidateSerialSettings({
            .baudRate = 12345,
            .dataBits = 8,
            .parity = mdv::SerialParity::None,
            .stopBits = 1,
        });
    }
    catch (const std::invalid_argument&) {
        badBaudRejected = true;
    }

    bool badDataBitsRejected = false;
    try {
        mdv::ValidateSerialSettings({
            .baudRate = 9600,
            .dataBits = 6,
            .parity = mdv::SerialParity::None,
            .stopBits = 1,
        });
    }
    catch (const std::invalid_argument&) {
        badDataBitsRejected = true;
    }

    return Check(badBaudRejected, "unsupported baud rejected") &&
        Check(badDataBitsRejected, "unsupported data bits rejected");
}

bool TestTransportDefaults()
{
    mdv::modbus::RtuSerialTransport transport;
    return Check(!transport.IsOpen(), "transport initially closed") &&
        Check(transport.Serial().baudRate == 9600, "default Modbus baud 9600") &&
        Check(transport.Serial().dataBits == 8, "default Modbus data bits 8") &&
        Check(
            transport.Serial().parity == mdv::SerialParity::None,
            "default Modbus parity none") &&
        Check(transport.Serial().stopBits == 1, "default Modbus stop bits 1") &&
        Check(
            transport.Timing().responseTimeout == std::chrono::milliseconds(200),
            "default response timeout");
}

bool TestInvalidTimingRejected()
{
    bool rejected = false;
    try {
        static_cast<void>(mdv::modbus::RtuSerialTransport(
            {}, mdv::modbus::RtuTimingSettings{std::chrono::milliseconds(0)}));
    }
    catch (const std::invalid_argument&) {
        rejected = true;
    }
    return Check(rejected, "zero Modbus timeout rejected");
}

bool TestClosedPortResult()
{
    mdv::modbus::RtuSerialTransport transport;
    const auto request = mdv::modbus::BuildReadHoldingRegistersRequest(1, 0x006B, 3);
    const auto result = transport.Execute(request);

    return Check(
               result.status == mdv::modbus::TransactionStatus::IoError,
               "closed port reports I/O error") &&
        Check(!result.error.empty(), "closed-port error text") &&
        Check(!result.response.has_value(), "closed port has no response");
}

bool TestInvalidRequestRejectedBeforeIo()
{
    mdv::modbus::RtuSerialTransport transport;
    auto request = mdv::modbus::BuildReadHoldingRegistersRequest(1, 0x006B, 3);
    request.back() ^= 0x01;

    const auto result = transport.Execute(request);
    return Check(
               result.status == mdv::modbus::TransactionStatus::InvalidRequest,
               "bad request CRC rejected before serial I/O") &&
        Check(!result.error.empty(), "bad request CRC error text");
}

} // namespace

int main()
{
    const bool ok = TestInterFrameDelay() &&
        TestResponseDeadlineStartsAfterWriteCompletion() &&
        TestSerialValidation() &&
        TestTransportDefaults() &&
        TestInvalidTimingRejected() &&
        TestClosedPortResult() &&
        TestInvalidRequestRejectedBeforeIo();

    if (ok) {
        std::cout << "Modbus RTU serial transport tests passed\n";
        return 0;
    }
    return 1;
}
