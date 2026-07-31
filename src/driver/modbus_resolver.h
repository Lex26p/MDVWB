#pragma once

#include "modbus_profile.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

namespace mdv::modbus {

class ResolverError final : public std::runtime_error {
public:
    explicit ResolverError(std::string message);
};

struct ResolvedDeviceAddress {
    std::uint8_t logicalAddress = 0;
    std::uint8_t slaveId = 1;
    std::uint16_t registerOffset = 0;
};

struct ResolvedRegisterLocation {
    std::uint8_t logicalAddress = 0;
    std::uint8_t slaveId = 1;
    RegisterSpace space = RegisterSpace::HoldingRegister;
    std::uint16_t address = 0;
};

// Invalid MDVWB logical addresses (outside 1..63) are errors.
// A valid 1..63 candidate that is not supported by this profile returns nullopt.
// For explicit addressing, a missing per-device entry therefore means
// "unsupported candidate", which scan code can skip deterministically.
[[nodiscard]] std::optional<ResolvedDeviceAddress> ResolveLogicalAddress(
    const ModbusProfile& profile,
    std::uint8_t logicalAddress);

// Applies the resolved profile register offset to one base point/probe location.
// Returns nullopt for a valid but unsupported logical candidate.
// Throws ResolverError if the effective 16-bit Modbus register would overflow.
[[nodiscard]] std::optional<ResolvedRegisterLocation> ResolveRegisterLocation(
    const ModbusProfile& profile,
    std::uint8_t logicalAddress,
    const RegisterLocation& baseLocation);

} // namespace mdv::modbus
