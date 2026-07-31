#pragma once

#include "modbus_profile.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace mdv::modbus {

class ValueConversionError final : public std::runtime_error {
public:
    explicit ValueConversionError(std::string message);
};

using PointValue = std::variant<bool, std::string, double>;

// Decode one raw 16-bit Modbus register value using one validated profile point.
// Composite/multi-register values are intentionally outside this milestone.
[[nodiscard]] PointValue DecodePointValue(
    const PointDefinition& point,
    std::uint16_t rawValue);

// Convert one semantic point value back into the 16-bit register representation.
// The caller is responsible for using point.write and for performing the bus write.
[[nodiscard]] std::uint16_t EncodePointValue(
    const PointDefinition& point,
    const PointValue& value);

} // namespace mdv::modbus
