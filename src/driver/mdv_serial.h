#pragma once

#include "mdv_protocol.h"
#include "serial_port.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mdv {

constexpr std::size_t kWireRequestSize = kRequestSize + 1;
using WireRequest = std::array<std::uint8_t, kWireRequestSize>;

[[nodiscard]] WireRequest BuildWireRequest(const RequestFrame& frame) noexcept;

struct TimingSettings {
    std::chrono::milliseconds transactionPeriod{150};
    std::chrono::milliseconds responseTimeout{130};
};

class TransactionPacer {
public:
    explicit TransactionPacer(TimingSettings settings = {});

    [[nodiscard]] std::chrono::steady_clock::time_point WaitForNextStart();
    [[nodiscard]] std::chrono::steady_clock::time_point ResponseDeadline(
        std::chrono::steady_clock::time_point transactionStart) const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point NextAllowedStart(
        std::chrono::steady_clock::time_point transactionStart) const noexcept;

    [[nodiscard]] const TimingSettings& Settings() const noexcept;
    void Reset() noexcept;

private:
    TimingSettings settings_{};
    std::optional<std::chrono::steady_clock::time_point> nextAllowedStart_;
};

enum class TransactionStatus {
    Success,
    Timeout,
    IoError,
};

struct TransactionResult {
    TransactionStatus status = TransactionStatus::Timeout;
    std::optional<ResponseFrame> response;
    std::string error;
    std::chrono::milliseconds elapsed{0};
};

class ITransactionTransport {
public:
    virtual ~ITransactionTransport() = default;

    [[nodiscard]] virtual TransactionResult Execute(const RequestFrame& request) = 0;
};

// Owns the only physical path to the RS-485 line. Every request type uses the
// same pacer, so C0, C3, CC and CD all follow one start-to-start period.
class MdvSerialTransport final : public ITransactionTransport {
public:
    explicit MdvSerialTransport(TimingSettings timing = {});

    void Open(std::string_view portName);
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

    [[nodiscard]] TransactionResult Execute(const RequestFrame& request) override;

    [[nodiscard]] const TimingSettings& Timing() const noexcept;
    [[nodiscard]] const std::string& PortName() const noexcept;

private:
    SerialPort port_;
    TransactionPacer pacer_;
};

} // namespace mdv
