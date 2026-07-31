#pragma once

#include "device_driver.h"
#include "mdv_device.h"
#include "mdv_serial.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace mdv {

inline constexpr std::uint32_t kMaxSetCommandAttempts = 3;
inline constexpr std::uint32_t kMaxBlockCommandAttempts = 3;
inline constexpr std::size_t kMaxPriorityOperationsBeforePoll = 4;

struct DeviceRuntime {
    explicit DeviceRuntime(std::uint8_t address, std::uint8_t masterId = 0)
        : device(address, masterId)
    {
    }

    DeviceContext device;
    bool online = false;
    bool setQueueEntry = false;
    bool blockQueueEntry = false;
    bool confirmQueueEntry = false;

    std::uint64_t setAttemptRevision = 0;
    std::uint32_t setAttempts = 0;
    bool setRetryExhausted = false;

    bool desiredBlocked = false;
    bool blockPending = false;
    std::uint64_t blockRevision = 0;
    std::uint64_t blockAttemptRevision = 0;
    std::uint32_t blockAttempts = 0;
    bool blockRetryExhausted = false;

    std::uint64_t successfulReads = 0;
    std::uint64_t failedReads = 0;
    std::uint64_t successfulSets = 0;
    std::uint64_t failedSets = 0;
    std::uint64_t successfulBlockCommands = 0;
    std::uint64_t failedBlockCommands = 0;
    std::uint32_t consecutiveReadFailures = 0;
    std::string lastError;
};

// Owns the transaction order for one RS-485 line. Confirmation reads have the
// highest priority, then CC/CD and cached C3. A bounded priority burst is
// followed by one ordinary round-robin C0 so one unconfirmed device cannot
// starve all other addresses.
class MdvDriver final : public IDeviceDriver {
public:
    MdvDriver(
        std::vector<std::uint8_t> addresses,
        ITransactionTransport& transport,
        std::uint8_t masterId = 0);

    [[nodiscard]] DriverResult ProcessNext() override;

    // Protocol-neutral command entry point. Existing strongly typed MDV
    // methods remain available below so this refactor does not alter current
    // behavior or force protocol details into callers that still use them.
    void ApplyCommand(const DriverCommand& command) override
    {
        switch (command.control) {
        case DriverControl::Power: {
            const auto* value = std::get_if<bool>(&command.value);
            if (value == nullptr) {
                throw std::invalid_argument("Power command requires a boolean value");
            }
            SetPower(command.address, *value);
            return;
        }

        case DriverControl::Mode: {
            const auto* value = std::get_if<HvacMode>(&command.value);
            if (value == nullptr) {
                throw std::invalid_argument("Mode command requires an HVAC mode value");
            }

            switch (*value) {
            case HvacMode::Cool:
                SetMode(command.address, Mode::Cool);
                return;
            case HvacMode::Heat:
                SetMode(command.address, Mode::Heat);
                return;
            case HvacMode::Dry:
                SetMode(command.address, Mode::Dry);
                return;
            case HvacMode::Fan:
                SetMode(command.address, Mode::Fan);
                return;
            case HvacMode::Auto:
                SetMode(command.address, Mode::Auto);
                return;
            }
            throw std::invalid_argument("unknown HVAC mode");
        }

        case DriverControl::FanSpeed: {
            const auto* value = std::get_if<HvacFanSpeed>(&command.value);
            if (value == nullptr) {
                throw std::invalid_argument(
                    "FanSpeed command requires an HVAC fan-speed value");
            }

            switch (*value) {
            case HvacFanSpeed::Low:
                SetFanSpeed(command.address, FanSpeed::Low);
                return;
            case HvacFanSpeed::Medium:
                SetFanSpeed(command.address, FanSpeed::Medium);
                return;
            case HvacFanSpeed::High:
                SetFanSpeed(command.address, FanSpeed::High);
                return;
            case HvacFanSpeed::Auto:
                SetFanSpeed(command.address, FanSpeed::Auto);
                return;
            }
            throw std::invalid_argument("unknown HVAC fan speed");
        }

        case DriverControl::SetTemperature: {
            const auto* value = std::get_if<double>(&command.value);
            if (value == nullptr) {
                throw std::invalid_argument(
                    "SetTemperature command requires a numeric value");
            }
            if (!std::isfinite(*value) || std::floor(*value) != *value ||
                *value < 0.0 || *value > 255.0) {
                throw std::invalid_argument(
                    "MDV SetTemperature requires a whole-degree value");
            }
            SetTemperature(command.address, static_cast<std::uint8_t>(*value));
            return;
        }

        case DriverControl::Blinds: {
            const auto* value = std::get_if<bool>(&command.value);
            if (value == nullptr) {
                throw std::invalid_argument("Blinds command requires a boolean value");
            }
            SetBlinds(command.address, *value);
            return;
        }

        case DriverControl::Blocked: {
            const auto* value = std::get_if<bool>(&command.value);
            if (value == nullptr) {
                throw std::invalid_argument("Blocked command requires a boolean value");
            }
            SetBlocked(command.address, *value);
            return;
        }
        }

        throw std::invalid_argument("unknown driver control");
    }

    void SetPower(std::uint8_t address, bool power);
    void SetMode(std::uint8_t address, Mode mode);
    void SetFanSpeed(std::uint8_t address, FanSpeed speed);
    void SetTemperature(std::uint8_t address, std::uint8_t temperature);
    void SetBlinds(std::uint8_t address, bool enabled);
    void SetBlocked(std::uint8_t address, bool blocked);

    [[nodiscard]] bool HasQueuedWork() const noexcept override;
    [[nodiscard]] std::size_t DeviceCount() const noexcept override;
    [[nodiscard]] std::uint8_t NextPollAddress() const noexcept override;

    [[nodiscard]] DeviceRuntime& DeviceByAddress(std::uint8_t address);
    [[nodiscard]] const DeviceRuntime& DeviceByAddress(std::uint8_t address) const;

private:
    [[nodiscard]] DriverResult ExecuteRead(
        DeviceRuntime& runtime,
        DriverOperation operation);
    [[nodiscard]] DriverResult ExecuteSet(DeviceRuntime& runtime);
    [[nodiscard]] DriverResult ExecuteBlock(DeviceRuntime& runtime);
    void ResetSetRetry(DeviceRuntime& runtime) noexcept;
    void ResetBlockRetry(DeviceRuntime& runtime) noexcept;
    void EnqueueSet(DeviceRuntime& runtime);
    void EnqueueBlock(DeviceRuntime& runtime);
    void EnqueueConfirmation(DeviceRuntime& runtime);
    [[nodiscard]] DeviceRuntime& PopSet();
    [[nodiscard]] DeviceRuntime& PopBlock();
    [[nodiscard]] DeviceRuntime& PopConfirmation();
    [[nodiscard]] DeviceRuntime& NextPollDevice() noexcept;
    void MarkReadSuccess(DeviceRuntime& runtime) noexcept;
    void MarkReadFailure(DeviceRuntime& runtime, std::string error);
    void MarkSetSuccess(DeviceRuntime& runtime) noexcept;
    void MarkSetFailure(DeviceRuntime& runtime, std::string error);
    void MarkBlockSuccess(DeviceRuntime& runtime) noexcept;
    void MarkBlockFailure(DeviceRuntime& runtime, std::string error);

    std::vector<DeviceRuntime> devices_;
    ITransactionTransport& transport_;
    std::uint8_t masterId_ = 0;
    std::size_t nextPollIndex_ = 0;
    std::size_t priorityOperations_ = 0;
    std::deque<std::uint8_t> setQueue_;
    std::deque<std::uint8_t> blockQueue_;
    std::deque<std::uint8_t> confirmationQueue_;
};

} // namespace mdv
