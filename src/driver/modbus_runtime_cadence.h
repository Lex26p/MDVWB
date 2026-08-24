#pragma once

#include "device_driver.h"

#include <chrono>

namespace mdv::modbus {

struct ModbusRuntimeCadence {
    std::chrono::milliseconds pollPeriod{300};
    std::chrono::milliseconds commandPeriod{300};
    std::chrono::milliseconds retryPeriod{500};
};

// Returns the minimum start-to-start period for the next runtime operation.
// The poll period is the lower bound for every successful operation, so a
// write/confirmation burst cannot accelerate the bus past ordinary polling.
// Failed operations use the larger of that lower bound and retry backoff.
[[nodiscard]] std::chrono::milliseconds ModbusOperationPeriod(
    const DriverResult& result,
    const ModbusRuntimeCadence& cadence) noexcept;

} // namespace mdv::modbus
