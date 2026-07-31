#pragma once

#include "modbus_rtu.h"
#include "serial_port.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace mdv::modbus {

struct RtuTimingSettings {
    std::chrono::milliseconds responseTimeout{200};
};

enum class TransactionStatus {
    Success,
    Exception,
    Timeout,
    IoError,
    InvalidRequest,
    InvalidResponse,
};

struct TransactionResult {
    TransactionStatus status = TransactionStatus::Timeout;
    std::optional<ParsedResponse> response;
    std::string error;
    std::chrono::milliseconds elapsed{0};
};

[[nodiscard]] std::chrono::microseconds CalculateInterFrameDelay(
    const SerialSettings& settings);

class ITransactionTransport {
public:
    virtual ~ITransactionTransport() = default;
    [[nodiscard]] virtual TransactionResult Execute(const RtuAdu& request) = 0;
};

class RtuSerialTransport final : public ITransactionTransport {
public:
    explicit RtuSerialTransport(
        SerialSettings serial = SerialSettings{
            .baudRate = 9600,
            .dataBits = 8,
            .parity = SerialParity::None,
            .stopBits = 1,
        },
        RtuTimingSettings timing = {});

    void Open(std::string_view portName);
    void Close() noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

    [[nodiscard]] TransactionResult Execute(const RtuAdu& request) override;

    [[nodiscard]] const SerialSettings& Serial() const noexcept;
    [[nodiscard]] const RtuTimingSettings& Timing() const noexcept;
    [[nodiscard]] const std::string& PortName() const noexcept;

private:
    SerialSettings serial_{};
    RtuTimingSettings timing_{};
    SerialPort port_;
    std::optional<std::chrono::steady_clock::time_point> nextAllowedStart_;
};

} // namespace mdv::modbus
