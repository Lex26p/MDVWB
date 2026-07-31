#include "modbus_value.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace mdv::modbus {
namespace {

[[noreturn]] void Fail(std::string message)
{
    throw ValueConversionError(std::move(message));
}

[[nodiscard]] double NumericTolerance(double value) noexcept
{
    return 1e-9 * std::max(1.0, std::abs(value));
}

[[nodiscard]] std::int32_t DecodeInteger(
    RawType rawType,
    std::uint16_t rawValue) noexcept
{
    if (rawType == RawType::UInt16) {
        return static_cast<std::int32_t>(rawValue);
    }

    if (rawValue <= 0x7FFFU) {
        return static_cast<std::int32_t>(rawValue);
    }
    return static_cast<std::int32_t>(rawValue) - 0x10000;
}

[[nodiscard]] std::uint16_t EncodeInteger(
    RawType rawType,
    std::int64_t value)
{
    if (rawType == RawType::UInt16) {
        if (value < 0 || value > 65535) {
            Fail("numeric raw value is outside uint16 range 0..65535");
        }
        return static_cast<std::uint16_t>(value);
    }

    if (value < -32768 || value > 32767) {
        Fail("numeric raw value is outside int16 range -32768..32767");
    }

    if (value >= 0) {
        return static_cast<std::uint16_t>(value);
    }

    return static_cast<std::uint16_t>(
        static_cast<std::int64_t>(0x10000) + value);
}

[[nodiscard]] const NumericTransform& EffectiveTransform(
    const PointDefinition& point) noexcept
{
    static const NumericTransform identity{};
    return point.transform.has_value() ? *point.transform : identity;
}

void ValidateNumberLimits(
    const PointDefinition& point,
    double physical)
{
    if (!point.limits.has_value()) {
        return;
    }

    const auto& limits = *point.limits;
    const auto tolerance = NumericTolerance(physical);

    if (limits.minimum.has_value() &&
        physical < *limits.minimum - tolerance) {
        Fail(
            "numeric semantic value is below minimum " +
            std::to_string(*limits.minimum));
    }

    if (limits.maximum.has_value() &&
        physical > *limits.maximum + tolerance) {
        Fail(
            "numeric semantic value is above maximum " +
            std::to_string(*limits.maximum));
    }

    if (limits.step.has_value()) {
        // A physical step is anchored at min when min is declared, otherwise 0.
        const double anchor = limits.minimum.value_or(0.0);
        const double steps = (physical - anchor) / *limits.step;
        const double nearest = std::round(steps);
        if (std::abs(steps - nearest) > NumericTolerance(steps)) {
            Fail(
                "numeric semantic value does not align with configured step " +
                std::to_string(*limits.step));
        }
    }
}

[[nodiscard]] std::int64_t ApplyRounding(
    double raw,
    WriteRounding rounding)
{
    if (!std::isfinite(raw)) {
        Fail("inverse numeric conversion produced a non-finite raw value");
    }

    double rounded = 0.0;
    switch (rounding) {
    case WriteRounding::Exact:
        rounded = std::round(raw);
        if (std::abs(raw - rounded) > NumericTolerance(raw)) {
            Fail("numeric semantic value cannot be represented exactly as an integer raw value");
        }
        break;
    case WriteRounding::Nearest:
        rounded = std::round(raw);
        break;
    case WriteRounding::Floor:
        rounded = std::floor(raw);
        break;
    case WriteRounding::Ceil:
        rounded = std::ceil(raw);
        break;
    }

    constexpr double minimum =
        static_cast<double>(std::numeric_limits<std::int64_t>::min());
    constexpr double maximum =
        static_cast<double>(std::numeric_limits<std::int64_t>::max());

    if (rounded < minimum || rounded > maximum) {
        Fail("rounded numeric raw value is outside supported integer range");
    }

    return static_cast<std::int64_t>(rounded);
}

} // namespace

ValueConversionError::ValueConversionError(std::string message)
    : std::runtime_error(std::move(message))
{
}

PointValue DecodePointValue(
    const PointDefinition& point,
    std::uint16_t rawValue)
{
    switch (point.type) {
    case PointType::Boolean:
        if (rawValue == 0U) {
            return false;
        }
        if (rawValue == 1U) {
            return true;
        }
        Fail(
            "boolean raw value must be exactly 0 or 1, got " +
            std::to_string(rawValue));

    case PointType::Enum: {
        const auto iterator = point.enumMappings.read.find(rawValue);
        if (iterator == point.enumMappings.read.end()) {
            Fail(
                "enum raw value " + std::to_string(rawValue) +
                " is not present in readMap");
        }
        return iterator->second;
    }

    case PointType::Number: {
        const auto integer = DecodeInteger(point.rawType, rawValue);
        const auto& transform = EffectiveTransform(point);
        const double physical =
            static_cast<double>(integer) * transform.scale +
            transform.offset;
        if (!std::isfinite(physical)) {
            Fail("numeric read conversion produced a non-finite semantic value");
        }
        return physical;
    }
    }

    Fail("unknown Modbus point type");
}

std::uint16_t EncodePointValue(
    const PointDefinition& point,
    const PointValue& value)
{
    switch (point.type) {
    case PointType::Boolean: {
        const auto* boolean = std::get_if<bool>(&value);
        if (boolean == nullptr) {
            Fail("boolean point requires a boolean semantic value");
        }
        return *boolean ? 1U : 0U;
    }

    case PointType::Enum: {
        const auto* semantic = std::get_if<std::string>(&value);
        if (semantic == nullptr) {
            Fail("enum point requires a string semantic value");
        }

        const auto iterator = point.enumMappings.write.find(*semantic);
        if (iterator == point.enumMappings.write.end()) {
            Fail(
                "enum semantic value '" + *semantic +
                "' is not present in writeMap");
        }
        return iterator->second;
    }

    case PointType::Number: {
        const auto* physical = std::get_if<double>(&value);
        if (physical == nullptr || !std::isfinite(*physical)) {
            Fail("number point requires a finite numeric semantic value");
        }

        ValidateNumberLimits(point, *physical);

        const auto& transform = EffectiveTransform(point);
        if (!std::isfinite(transform.scale) || transform.scale == 0.0 ||
            !std::isfinite(transform.offset)) {
            Fail("numeric point has an invalid transform");
        }

        const double raw =
            (*physical - transform.offset) / transform.scale;
        const auto rounded = ApplyRounding(raw, point.rounding);
        return EncodeInteger(point.rawType, rounded);
    }
    }

    Fail("unknown Modbus point type");
}

} // namespace mdv::modbus
