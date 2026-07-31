#pragma once

#include "device_driver.h"
#include "modbus_profile.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mdv::modbus {

class SemanticConversionError final : public std::runtime_error {
public:
    explicit SemanticConversionError(std::string message);
};

struct EncodedSemanticWrite {
    DriverControl control = DriverControl::Power;
    RegisterLocation location;
    std::uint16_t rawValue = 0;
};

// Reports whether one profile point is exposed by the profile's declared
// capabilities. A point may exist for internal/profile reasons without being
// exposed to the common application model.
[[nodiscard]] bool IsSemanticPointEnabled(
    const ModbusProfile& profile,
    std::string_view pointName) noexcept;

// Decodes one raw register value and applies it to the protocol-independent
// device state. The caller owns online/hasState lifecycle and read batching.
void ApplySemanticRead(
    DriverDeviceState& state,
    const ModbusProfile& profile,
    std::string_view pointName,
    std::uint16_t rawValue);

// Converts a protocol-independent application command into one profile-defined
// base register write. Logical-address resolution/offsetting is intentionally
// left for Milestone 5.
[[nodiscard]] EncodedSemanticWrite EncodeSemanticWrite(
    const ModbusProfile& profile,
    DriverControl control,
    const DriverCommandValue& value);

} // namespace mdv::modbus
