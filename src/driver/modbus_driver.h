#pragma once

#include "device_driver.h"
#include "modbus_profile.h"
#include "modbus_rtu_serial.h"
#include "modbus_scan_execute.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mdv::modbus {

// Profile-driven Modbus implementation of the protocol-neutral device boundary.
//
// Milestone 9 step 1 is deliberately read-only. ProcessNext() performs one
// complete factual snapshot for one logical device. ApplyCommand() rejects all
// writes until the command/confirmation path is added in the next step.
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
    struct DeviceRuntime {
        std::uint8_t logicalAddress = 0;
        ScanProbe probe;
        DriverDeviceState state;
    };

    struct RawReadResult {
        DriverOutcome outcome = DriverOutcome::InvalidResponse;
        bool success = false;
        std::uint16_t value = 0;
        std::string error;
    };

    [[nodiscard]] DeviceRuntime& DeviceByAddress(std::uint8_t address);
    [[nodiscard]] const DeviceRuntime& DeviceByAddress(
        std::uint8_t address) const;

    [[nodiscard]] DriverResult Poll(DeviceRuntime& runtime);
    [[nodiscard]] RawReadResult ReadSemanticRegister(
        std::uint8_t logicalAddress,
        const RegisterLocation& baseLocation);

    [[nodiscard]] DriverResult MarkOffline(
        DeviceRuntime& runtime,
        DriverOutcome outcome,
        std::string error);

    void ValidateReadablePoints(std::uint8_t logicalAddress) const;

    ModbusProfile profile_;
    ITransactionTransport& transport_;
    std::vector<DeviceRuntime> devices_;
    std::size_t nextPollIndex_ = 0;
};

} // namespace mdv::modbus
