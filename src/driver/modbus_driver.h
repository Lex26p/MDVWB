#pragma once

#include "device_driver.h"
#include "modbus_profile.h"
#include "modbus_rtu_serial.h"
#include "modbus_scan_execute.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace mdv::modbus {

inline constexpr std::uint32_t kMaxModbusWriteAttempts = 3;
inline constexpr std::uint32_t kMaxModbusConfirmationAttempts = 3;
inline constexpr std::size_t kMaxModbusPriorityOperationsBeforePoll = 4;

// Profile-driven Modbus implementation of the protocol-neutral device boundary.
//
// Polling builds atomic factual snapshots. Supported commands are encoded by
// the selected profile, written through FC10 and become factual only after a
// read-back from the profile's read location confirms the requested value.
class ModbusDriver final : public IDeviceDriver {
public:
    ModbusDriver(
        std::vector<std::uint8_t> logicalAddresses,
        const ModbusProfile& profile,
        ITransactionTransport& transport);

    [[nodiscard]] DriverResult ProcessNext() override;
    void ApplyCommand(const DriverCommand& command) override;

    [[nodiscard]] DriverDeviceState DeviceStateByAddress(
        std::uint8_t address) const override;

    [[nodiscard]] bool HasQueuedWork() const noexcept override;
    [[nodiscard]] std::size_t DeviceCount() const noexcept override;
    [[nodiscard]] std::uint8_t NextPollAddress() const noexcept override;

private:
    struct PendingPower {
        bool desired = false;
        std::uint16_t rawValue = 0;
        std::uint8_t slaveId = 1;
        std::uint16_t writeAddress = 0;
        std::uint64_t revision = 0;
        std::uint32_t writeAttempts = 0;
        std::uint32_t confirmationAttempts = 0;
    };

    struct DeviceRuntime {
        std::uint8_t logicalAddress = 0;
        ScanProbe probe;
        DriverDeviceState state;
        std::optional<PendingPower> pendingPower;
    };

    struct RawReadResult {
        DriverOutcome outcome = DriverOutcome::InvalidResponse;
        bool success = false;
        std::uint16_t value = 0;
        std::string error;
    };

    struct WorkItem {
        std::uint8_t logicalAddress = 0;
        std::uint64_t revision = 0;
    };

    [[nodiscard]] DeviceRuntime& DeviceByAddress(std::uint8_t address);
    [[nodiscard]] const DeviceRuntime& DeviceByAddress(
        std::uint8_t address) const;

    [[nodiscard]] DriverResult Poll(DeviceRuntime& runtime);
    [[nodiscard]] DriverResult ExecutePowerWrite(DeviceRuntime& runtime);
    [[nodiscard]] DriverResult ConfirmPowerWrite(DeviceRuntime& runtime);

    [[nodiscard]] RawReadResult ReadSemanticRegister(
        std::uint8_t logicalAddress,
        const RegisterLocation& baseLocation);

    [[nodiscard]] DriverResult MarkOffline(
        DeviceRuntime& runtime,
        DriverOperation operation,
        DriverOutcome outcome,
        std::string error);

    void ValidateReadablePoints(std::uint8_t logicalAddress) const;
    void EnqueuePowerWrite(const DeviceRuntime& runtime);
    void EnqueuePowerConfirmation(const DeviceRuntime& runtime);

    [[nodiscard]] DeviceRuntime* PopValidWork(
        std::deque<WorkItem>& queue);

    [[nodiscard]] DriverResult ProcessPoll();

    ModbusProfile profile_;
    ITransactionTransport& transport_;
    std::vector<DeviceRuntime> devices_;
    std::deque<WorkItem> powerWriteQueue_;
    std::deque<WorkItem> powerConfirmationQueue_;
    std::size_t nextPollIndex_ = 0;
    std::size_t priorityOperations_ = 0;
    std::uint64_t nextCommandRevision_ = 0;
};

} // namespace mdv::modbus
