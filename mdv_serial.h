#pragma once

#include "mdv_protocol.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

class SerialPort {
public:
    SerialPort() = default;
    ~SerialPort();

    SerialPort(const SerialPort&) = delete;
    SerialPort& operator=(const SerialPort&) = delete;

    SerialPort(SerialPort&& other) noexcept;
    SerialPort& operator=(SerialPort&& other) noexcept;

    void Open(std::string_view portName);
    void Close() noexcept;

    [[nodiscard]] bool IsOpen() const noexcept;
    [[nodiscard]] const std::string& PortName() const noexcept;

    void DiscardInput();
    void WriteAll(std::span<const std::uint8_t> data);
    [[nodiscard]] std::size_t ReadSome(std::span<std::uint8_t> buffer);

    [[nodiscard]] static std::string NormalizePortName(std::string_view portName);

private:
#ifdef _WIN32
    void* handle_ = reinterpret_cast<void*>(-1);
#else
    int handle_ = -1;
#endif
    std::string portName_;
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

// Owns the only physical path to the RS-485 line. Every request type uses the
// same pacer, so C0, C3, CC and CD all follow one start-to-start period.
class MdvSerialTransport {
public:
    explicit MdvSerialTransport(TimingSettings timing = {});

    void Open(std::string_view portName);
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

    [[nodiscard]] TransactionResult Execute(const RequestFrame& request);

    [[nodiscard]] const TimingSettings& Timing() const noexcept;
    [[nodiscard]] const std::string& PortName() const noexcept;

private:
    SerialPort port_;
    TransactionPacer pacer_;
};

} // namespace mdv
