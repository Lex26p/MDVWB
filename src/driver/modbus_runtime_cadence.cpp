#include "modbus_runtime_cadence.h"

#include <algorithm>

namespace mdv::modbus {

std::chrono::milliseconds ModbusOperationPeriod(
    const DriverResult& result,
    const ModbusRuntimeCadence& cadence) noexcept
{
    if (result.outcome != DriverOutcome::Success) {
        return std::max(cadence.pollPeriod, cadence.retryPeriod);
    }
    if (result.operation == DriverOperation::PollRead) {
        return cadence.pollPeriod;
    }
    return std::max(cadence.pollPeriod, cadence.commandPeriod);
}

} // namespace mdv::modbus
