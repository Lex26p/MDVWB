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
    std::size_t batchIndex = 0;
    std::uint16_t registerOffset = 0;
};

// One read-only FC03 request. A batch contains only exact duplicate or directly
// adjacent holding registers on the same slave. Gaps are never crossed, because
// profiles do not declare unmodelled registers safe to read.
struct ModbusSemanticReadBatch {
    std::uint8_t slaveId = 1;
    std::uint16_t startAddress = 0;
    std::uint16_t quantity = 1;
};

struct ModbusDevicePollPlan {
    std::uint8_t logicalAddress = 0;
    ScanProbe probe;
    std::vector<ModbusResolvedSemanticRead> semanticReads;
    std::vector<ModbusSemanticReadBatch> semanticBatches;
    std::optional<ResolvedRegisterLocation> powerRead;
};

// Deterministic counters for one complete round-robin cycle. The baseline
// fields describe the established one-request-per-semantic-point path. The
// optimized fields describe the exact batches executed by the driver.
struct ModbusPollPlanMetrics {
    std::size_t deviceCount = 0;
    std::size_t probeTransactionsPerCycle = 0;
    std::size_t semanticTransactionsPerCycle = 0;
    std::size_t totalTransactionsPerCycle = 0;
    std::size_t registersRequestedPerCycle = 0;

    std::size_t optimizedSemanticTransactionsPerCycle = 0;
    std::size_t optimizedTotalTransactionsPerCycle = 0;
    std::size_t optimizedRegistersRequestedPerCycle = 0;
    std::size_t reusedSemanticReadsPerCycle = 0;
    std::size_t savedTransactionsPerCycle = 0;
};

struct ModbusPollPlan {
    std::vector<ModbusDevicePollPlan> devices;
    ModbusPollPlanMetrics metrics;
};

// Resolves every read location exactly once during driver construction. The
// returned plan performs no I/O, preserves semantic application order, and
// combines only duplicate/directly-adjacent FC03 holding-register reads.
[[nodiscard]] ModbusPollPlan BuildModbusPollPlan(
    const ModbusProfile& profile,
    const std::vector<std::uint8_t>& logicalAddresses);

} // namespace mdv::modbus
