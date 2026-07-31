#include "mdv_serial.h"

#include <algorithm>
#include <stdexcept>
#include <thread>

namespace mdv {
namespace {

void ValidateTiming(const TimingSettings& settings)
{
    if (settings.transactionPeriod.count() <= 0) {
        throw std::invalid_argument("MDV transaction period must be positive");
    }
    if (settings.responseTimeout.count() <= 0) {
        throw std::invalid_argument("MDV response timeout must be positive");
    }
    if (settings.responseTimeout >= settings.transactionPeriod) {
        throw std::invalid_argument(
            "MDV response timeout must be shorter than transaction period");
    }
}

[[nodiscard]] SerialSettings MdvSerialSettings() noexcept
{
    return SerialSettings{
        .baudRate = 4800,
        .dataBits = 8,
        .parity = SerialParity::None,
        .stopBits = 1,
    };
}

} // namespace

WireRequest BuildWireRequest(const RequestFrame& frame) noexcept
{
    WireRequest wire{};
    wire[0] = kTransportPadding;
    std::copy(frame.begin(), frame.end(), wire.begin() + 1);
    return wire;
}

TransactionPacer::TransactionPacer(TimingSettings settings)
    : settings_(settings)
{
    ValidateTiming(settings_);
}

std::chrono::steady_clock::time_point TransactionPacer::WaitForNextStart()
{
    if (nextAllowedStart_.has_value()) {
        std::this_thread::sleep_until(*nextAllowedStart_);
    }

    const auto actualStart = std::chrono::steady_clock::now();
    nextAllowedStart_ = NextAllowedStart(actualStart);
    return actualStart;
}

std::chrono::steady_clock::time_point TransactionPacer::ResponseDeadline(
    std::chrono::steady_clock::time_point transactionStart) const noexcept
{
    return transactionStart + settings_.responseTimeout;
}

std::chrono::steady_clock::time_point TransactionPacer::NextAllowedStart(
    std::chrono::steady_clock::time_point transactionStart) const noexcept
{
    return transactionStart + settings_.transactionPeriod;
}

const TimingSettings& TransactionPacer::Settings() const noexcept
{
    return settings_;
}

void TransactionPacer::Reset() noexcept
{
    nextAllowedStart_.reset();
}

MdvSerialTransport::MdvSerialTransport(TimingSettings timing)
    : pacer_(timing)
{
}

void MdvSerialTransport::Open(std::string_view portName)
{
    port_.Open(portName, MdvSerialSettings());
    pacer_.Reset();
}

void MdvSerialTransport::Close() noexcept
{
    port_.Close();
    pacer_.Reset();
}

bool MdvSerialTransport::IsOpen() const noexcept
{
    return port_.IsOpen();
}

TransactionResult MdvSerialTransport::Execute(const RequestFrame& request)
{
    TransactionResult result;
    if (!port_.IsOpen()) {
        result.status = TransactionStatus::IoError;
        result.error = "serial port is not open";
        return result;
    }

    const auto startedAt = pacer_.WaitForNextStart();
    const auto deadline = pacer_.ResponseDeadline(startedAt);

    try {
        port_.DiscardInput();
        const auto wireRequest = BuildWireRequest(request);
        port_.WriteAll(wireRequest);

        ResponseFrameCollector collector;
        std::array<std::uint8_t, 64> buffer{};

        while (std::chrono::steady_clock::now() < deadline) {
            const auto count = port_.ReadSome(buffer);
            for (std::size_t index = 0; index < count; ++index) {
                if (auto response = collector.Push(buffer[index]); response.has_value()) {
                    result.status = TransactionStatus::Success;
                    result.response = *response;
                    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - startedAt);
                    return result;
                }
            }

            if (count == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        result.status = TransactionStatus::Timeout;
        result.error = "MDV response timeout";
    }
    catch (const std::exception& exception) {
        result.status = TransactionStatus::IoError;
        result.error = exception.what();
    }

    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - startedAt);
    return result;
}

const TimingSettings& MdvSerialTransport::Timing() const noexcept
{
    return pacer_.Settings();
}

const std::string& MdvSerialTransport::PortName() const noexcept
{
    return port_.PortName();
}

} // namespace mdv
