#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace mdv {

// Existing MDV installations can use address 0, therefore the common boundary
// preserves 0..63. A future Modbus bus applies its stricter project rule 1..63
// in the Modbus resolver/configuration layer.
inline constexpr std::uint8_t kMaxLogicalDeviceAddress = 0x3F;

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

// State exposed to protocol-independent consumers such as MQTT. Values are
// optional where a protocol/device may not support or confirm the property.
struct DriverDeviceState {
    std::uint8_t address = 0;
    bool online = false;
    bool hasState = false;

    bool power = false;
    std::optional<HvacMode> mode;
    std::optional<HvacMode> activeMode;
    std::optional<HvacFanSpeed> fanSpeed;
    std::optional<HvacFanSpeed> activeFanSpeed;
    std::optional<double> setTemperature;
    std::optional<double> roomTemperature;

    std::optional<bool> blinds;
    std::optional<bool> blocked;
    int alarmCode = 0;
};

// Small protocol-independent boundary used by the application loop, MQTT and
// future protocol implementations.
class IDeviceDriver {
public:
    virtual ~IDeviceDriver() = default;

    [[nodiscard]] virtual DriverResult ProcessNext() = 0;
    virtual void ApplyCommand(const DriverCommand& command) = 0;

    [[nodiscard]] virtual DriverDeviceState DeviceStateByAddress(
        std::uint8_t address) const = 0;

    [[nodiscard]] virtual bool HasQueuedWork() const noexcept = 0;
    [[nodiscard]] virtual std::size_t DeviceCount() const noexcept = 0;
    [[nodiscard]] virtual std::uint8_t NextPollAddress() const noexcept = 0;
};

} // namespace mdv
