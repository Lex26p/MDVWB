#include "modbus_rtu_serial.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <utility>

namespace mdv::modbus {
namespace {

void ValidateTiming(const RtuTimingSettings& timing)
{
    if (timing.responseTimeout.count() <= 0) {
        throw std::invalid_argument("Modbus response timeout must be positive");
    }
}

struct RequestMetadata {
    std::uint8_t slaveId = 0;
    Function function = Function::ReadHoldingRegisters;
};

[[nodiscard]] std::optional<RequestMetadata> RequestMetadataFromAdu(
    const RtuAdu& request,
    std::string& error)
{
    if (request.size() < 4 || request.size() > kMaxRtuAduSize) {
        error = "Modbus RTU request length is invalid";
        return std::nullopt;
    }
    if (!HasValidCrc(request)) {
        error = "Modbus RTU request CRC is invalid";
        return std::nullopt;
    }
    if (request[0] < kMinSlaveId || request[0] > kMaxSlaveId) {
        error = "Modbus slave ID must be in range 1..247";
        return std::nullopt;
    }

    Function function;
    switch (request[1]) {
    case static_cast<std::uint8_t>(Function::ReadHoldingRegisters):
        function = Function::ReadHoldingRegisters;
        if (request.size() != 8) {
            error = "Modbus FC03 request must contain exactly 8 bytes";
            return std::nullopt;
        }
        break;
    case static_cast<std::uint8_t>(Function::WriteMultipleRegisters):
        function = Function::WriteMultipleRegisters;
        if (request.size() < 11) {
            error = "Modbus FC10 request is too short";
            return std::nullopt;
        }
        if (request[6] != request.size() - 9U) {
            error = "Modbus FC10 request byte count does not match payload";
            return std::nullopt;
        }
        break;
    default:
        error = "unsupported Modbus RTU function";
        return std::nullopt;
    }

    return RequestMetadata{request[0], function};
}

[[nodiscard]] std::string ExceptionError(const ParsedResponse& response)
{
    if (!response.exceptionCode.has_value()) {
        return "Modbus exception response has no exception code";
    }
    return "Modbus exception code " + std::to_string(*response.exceptionCode);
}

} // namespace

std::chrono::microseconds CalculateInterFrameDelay(const SerialSettings& settings)
{
    ValidateSerialSettings(settings);

    // Modbus Serial Line specification uses a fixed 1.75 ms t3.5 above
    // 19200 baud. At lower rates calculate 3.5 character times.
    if (settings.baudRate > 19200) {
        return std::chrono::microseconds(1750);
    }

    const std::uint64_t parityBits =
        settings.parity == SerialParity::None ? 0U : 1U;
    const std::uint64_t bitsPerCharacter =
        1U + settings.dataBits + parityBits + settings.stopBits;

    // ceil(bitsPerCharacter * 3.5 * 1,000,000 / baudRate)
    const std::uint64_t numerator = bitsPerCharacter * 35U * 1000000U;
    const std::uint64_t denominator =
        static_cast<std::uint64_t>(settings.baudRate) * 10U;
    const std::uint64_t microseconds =
        (numerator + denominator - 1U) / denominator;
    return std::chrono::microseconds(microseconds);
}

std::chrono::steady_clock::time_point CalculateResponseDeadline(
    std::chrono::steady_clock::time_point writeCompletedAt,
    const RtuTimingSettings& timing)
{
    ValidateTiming(timing);
    return writeCompletedAt + timing.responseTimeout;
}

RtuSerialTransport::RtuSerialTransport(
    SerialSettings serial,
    RtuTimingSettings timing)
    : serial_(serial), timing_(timing)
{
    ValidateSerialSettings(serial_);
    ValidateTiming(timing_);
}

void RtuSerialTransport::Open(std::string_view portName)
{
    port_.Open(portName, serial_);
    nextAllowedStart_.reset();
}

void RtuSerialTransport::Close() noexcept
{
    port_.Close();
    nextAllowedStart_.reset();
}

bool RtuSerialTransport::IsOpen() const noexcept
{
    return port_.IsOpen();
}

TransactionResult RtuSerialTransport::Execute(const RtuAdu& request)
{
    TransactionResult result;

    std::string requestError;
    const auto metadata = RequestMetadataFromAdu(request, requestError);
    if (!metadata.has_value()) {
        result.status = TransactionStatus::InvalidRequest;
        result.error = std::move(requestError);
        return result;
    }

    if (!port_.IsOpen()) {
        result.status = TransactionStatus::IoError;
        result.error = "serial port is not open";
        return result;
    }

    if (nextAllowedStart_.has_value()) {
        std::this_thread::sleep_until(*nextAllowedStart_);
    }

    const auto startedAt = std::chrono::steady_clock::now();

    const auto finish = [this, startedAt](TransactionResult completed) {
        const auto now = std::chrono::steady_clock::now();
        completed.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - startedAt);
        nextAllowedStart_ = now + CalculateInterFrameDelay(serial_);
        return completed;
    };

    try {
        port_.DiscardInput();
        port_.WriteAll(request);

        const auto deadline = CalculateResponseDeadline(
            std::chrono::steady_clock::now(),
            timing_);

        ResponseCollector collector;
        std::array<std::uint8_t, 256> buffer{};

        while (std::chrono::steady_clock::now() < deadline) {
            const auto count = port_.ReadSome(buffer);
            for (std::size_t index = 0; index < count; ++index) {
                auto adu = collector.Push(buffer[index]);
                if (!adu.has_value()) {
                    continue;
                }

                const auto parsed = ParseResponse(
                    *adu, metadata->slaveId, metadata->function);
                result.response = parsed;
                switch (parsed.status) {
                case ResponseStatus::Success:
                    result.status = TransactionStatus::Success;
                    return finish(std::move(result));
                case ResponseStatus::Exception:
                    result.status = TransactionStatus::Exception;
                    result.error = ExceptionError(parsed);
                    return finish(std::move(result));
                case ResponseStatus::Invalid:
                    result.status = TransactionStatus::InvalidResponse;
                    result.error = parsed.error;
                    return finish(std::move(result));
                }
            }

            if (count == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        result.status = TransactionStatus::Timeout;
        result.error = "Modbus response timeout";
        return finish(std::move(result));
    }
    catch (const std::exception& exception) {
        result.status = TransactionStatus::IoError;
        result.error = exception.what();
        return finish(std::move(result));
    }
}

const SerialSettings& RtuSerialTransport::Serial() const noexcept
{
    return serial_;
}

const RtuTimingSettings& RtuSerialTransport::Timing() const noexcept
{
    return timing_;
}

const std::string& RtuSerialTransport::PortName() const noexcept
{
    return port_.PortName();
}

} // namespace mdv::modbus
