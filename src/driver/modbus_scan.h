#pragma once

#include "modbus_profile.h"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mdv::modbus {

class ScanPlanError final : public std::runtime_error {
public:
    explicit ScanPlanError(std::string message);
};

struct ScanProbe {
    std::uint8_t logicalAddress = 0;
    std::uint8_t slaveId = 1;
    RegisterSpace space = RegisterSpace::HoldingRegister;
    std::uint16_t address = 0;
    std::uint16_t quantity = 1;
    ProbePresence presence = ProbePresence::AnyResponse;
};

struct ScanCandidate {
    std::uint8_t logicalAddress = 0;
    std::optional<ScanProbe> probe;

    [[nodiscard]] bool Supported() const noexcept
    {
        return probe.has_value();
    }
};

using ScanPlan = std::vector<ScanCandidate>;

// Builds a deterministic 1..63 scan plan, including 0 when the profile allows it.
//
// Every MDVWB logical candidate is represented exactly once. Candidates that
// the profile cannot resolve are retained with probe == nullopt so later scan
// execution can report them as unsupported without touching the bus.
//
// This function performs no I/O and never writes to Modbus.
[[nodiscard]] ScanPlan BuildScanPlan(const ModbusProfile& profile);

} // namespace mdv::modbus
