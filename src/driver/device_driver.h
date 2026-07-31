#pragma once

#include "mdv_protocol.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>

namespace mdv {

// Protocol-neutral operation/result vocabulary used by the application layer.
// The concrete MDV driver and future Modbus driver expose the same lifecycle.
enum class DriverOperation {
    PollRead,
    SetState,
    Lock,
    Unlock,
    ConfirmRead,
};

enum class DriverOutcome {
    Success,
    Timeout,
    IoError,
    InvalidResponse,
};

struct DriverResult {
    std::uint8_t address = 0;
    DriverOperation operation = DriverOperation::PollRead;
    DriverOutcome outcome = DriverOutcome::Timeout;
    std::string error;
};

// Semantic HVAC values. These are intentionally independent from MQTT numeric
// values and from protocol-specific wire encodings.
enum class HvacMode {
    Cool,
    Heat,
    Dry,
    Fan,
    Auto,
};

enum class HvacFanSpeed {
    Low,
    Medium,
    High,
    Auto,
};

enum class DriverControl {
    Power,
    Mode,
    FanSpeed,
    SetTemperature,
    Blinds,
    Blocked,
};

using DriverCommandValue = std::variant<
    bool,
    HvacMode,
    HvacFanSpeed,
    double>;

struct DriverCommand {
    std::uint8_t address = 0;
    DriverControl control = DriverControl::Power;
    DriverCommandValue value = false;
};

// Small protocol-independent boundary used by the application loop and command
// sources. State publication is deliberately not included yet: the current MDV
// runtime exposes protocol-specific DeviceContext internals, and that part will
// be separated in the next step instead of leaking it into this interface.
class IDeviceDriver {
public:
    virtual ~IDeviceDriver() = default;

    [[nodiscard]] virtual DriverResult ProcessNext() = 0;
    virtual void ApplyCommand(const DriverCommand& command) = 0;

    [[nodiscard]] virtual bool HasQueuedWork() const noexcept = 0;
    [[nodiscard]] virtual std::size_t DeviceCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint8_t NextPollAddress() const noexcept = 0;
};

} // namespace mdv
