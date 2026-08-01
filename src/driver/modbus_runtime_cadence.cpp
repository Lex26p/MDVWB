#include "modbus_runtime_cadence.h"

namespace mdv::modbus {

std::chrono::milliseconds ModbusOperationPeriod(
    const DriverResult& result,
    const ModbusRuntimeCadence& cadence) noexcept
{
    if (result.outcome != DriverOutcome::Success) {
        return cadence.retryPeriod;
    }
    if (result.operation == DriverOperation::PollRead) {
        return cadence.pollPeriod;
    }
    return cadence.commandPeriod;
}

} // namespace mdv::modbus
