#pragma once

#include "device_driver.h"

#include <chrono>

namespace mdv::modbus {

struct ModbusRuntimeCadence {
    std::chrono::milliseconds pollPeriod{150};
    std::chrono::milliseconds commandPeriod{20};
    std::chrono::milliseconds retryPeriod{500};
};

// Returns the minimum start-to-start period for the next runtime operation.
// Successful ordinary polls keep the configured poll cadence, successful
// command/write-confirmation work gets a shorter latency-oriented cadence, and
// every failed operation is rate-limited by the retry period.
[[nodiscard]] std::chrono::milliseconds ModbusOperationPeriod(
    const DriverResult& result,
    const ModbusRuntimeCadence& cadence) noexcept;

} // namespace mdv::modbus
