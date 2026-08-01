#pragma once

#include "modbus_profile.h"
#include "modbus_resolver.h"
#include "modbus_scan.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mdv::modbus {

struct ModbusResolvedSemanticRead {
    std::string pointName;
    ResolvedRegisterLocation location;
};

struct ModbusDevicePollPlan {
    std::uint8_t logicalAddress = 0;
    ScanProbe probe;
    std::vector<ModbusResolvedSemanticRead> semanticReads;
    std::optional<ResolvedRegisterLocation> powerRead;
};

// Deterministic baseline for one complete round-robin cycle. These counters
// describe the established non-batched path and make later optimizations
// measurable without involving wall-clock timing or real hardware.
struct ModbusPollPlanMetrics {
    std::size_t deviceCount = 0;
    std::size_t probeTransactionsPerCycle = 0;
    std::size_t semanticTransactionsPerCycle = 0;
    std::size_t totalTransactionsPerCycle = 0;
    std::size_t registersRequestedPerCycle = 0;
};

struct ModbusPollPlan {
    std::vector<ModbusDevicePollPlan> devices;
    ModbusPollPlanMetrics metrics;
};

// Resolves every read location exactly once during driver construction. The
// returned plan performs no I/O and preserves the profile's semantic order.
[[nodiscard]] ModbusPollPlan BuildModbusPollPlan(
    const ModbusProfile& profile,
    const std::vector<std::uint8_t>& logicalAddresses);

} // namespace mdv::modbus
