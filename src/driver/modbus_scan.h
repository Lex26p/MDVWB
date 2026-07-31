#pragma once

#include "modbus_profile.h"

#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

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

using ScanPlan = std::array<
    ScanCandidate,
    static_cast<std::size_t>(kMaxLogicalAddress)>;

// Builds a deterministic 1..63 scan plan from the selected profile.
//
// Every MDVWB logical candidate is represented exactly once. Candidates that
// the profile cannot resolve are retained with probe == nullopt so later scan
// execution can report them as unsupported without touching the bus.
//
// This function performs no I/O and never writes to Modbus.
[[nodiscard]] ScanPlan BuildScanPlan(const ModbusProfile& profile);

} // namespace mdv::modbus
