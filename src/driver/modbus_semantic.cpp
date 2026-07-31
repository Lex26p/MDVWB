#include "modbus_semantic.h"

#include "modbus_value.h"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>

namespace mdv::modbus {
namespace {

[[noreturn]] void Fail(std::string message)
{
    throw SemanticConversionError(std::move(message));
}

[[nodiscard]] const PointDefinition& RequirePoint(
    const ModbusProfile& profile,
    std::string_view pointName)
{
    const auto iterator = profile.points.find(pointName);
    if (iterator == profile.points.end()) {
        Fail(
            "profile '" + profile.id +
            "' does not define semantic point '" +
            std::string(pointName) + "'");
    }
    return iterator->second;
}

[[nodiscard]] std::string_view ModeName(HvacMode mode) noexcept
{
    switch (mode) {
    case HvacMode::Cool:
        return "cool";
    case HvacMode::Heat:
        return "heat";
    case HvacMode::Dry:
        return "dry";
    case HvacMode::Fan:
        return "fan";
    case HvacMode::Auto:
        return "auto";
    }
    return {};
}

[[nodiscard]] HvacMode ParseMode(std::string_view value)
{
    if (value == "cool") {
        return HvacMode::Cool;
    }
    if (value == "heat") {
        return HvacMode::Heat;
    }
    if (value == "dry") {
        return HvacMode::Dry;
    }
    if (value == "fan") {
        return HvacMode::Fan;
    }
    if (value == "auto") {
        return HvacMode::Auto;
    }
    Fail("unsupported semantic HVAC mode '" + std::string(value) + "'");
}

[[nodiscard]] std::string_view FanSpeedName(HvacFanSpeed speed) noexcept
{
    switch (speed) {
    case HvacFanSpeed::Low:
        return "low";
    case HvacFanSpeed::Medium:
        return "medium";
    case HvacFanSpeed::High:
        return "high";
    case HvacFanSpeed::Auto:
        return "auto";
    }
    return {};
}

[[nodiscard]] HvacFanSpeed ParseFanSpeed(std::string_view value)
{
    if (value == "low") {
        return HvacFanSpeed::Low;
    }
    if (value == "medium") {
        return HvacFanSpeed::Medium;
    }
    if (value == "high") {
        return HvacFanSpeed::High;
    }
    if (value == "auto") {
        return HvacFanSpeed::Auto;
    }
    Fail("unsupported semantic HVAC fan speed '" + std::string(value) + "'");
}

[[nodiscard]] bool DecodeBinarySemantic(
    std::string_view pointName,
    const PointValue& value)
{
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean;
    }

    const auto* text = std::get_if<std::string>(&value);
    if (text == nullptr) {
        Fail(
            "semantic point '" + std::string(pointName) +
            "' must decode to boolean or enum text");
    }

    if (pointName == "power") {
        if (*text == "off") {
            return false;
        }
        if (*text == "on") {
            return true;
        }
        Fail("power enum must use semantic values 'off' and 'on'");
    }

    if (pointName == "blinds") {
        if (*text == "disabled") {
            return false;
        }
        if (*text == "enabled") {
            return true;
        }
        Fail("blinds enum must use semantic values 'disabled' and 'enabled'");
    }

    if (pointName == "blocked") {
        if (*text == "unblocked") {
            return false;
        }
        if (*text == "blocked") {
            return true;
        }
        Fail("blocked enum must use semantic values 'unblocked' and 'blocked'");
    }

    Fail("unsupported binary semantic point '" + std::string(pointName) + "'");
}

[[nodiscard]] PointValue EncodeBinarySemantic(
    std::string_view pointName,
    const PointDefinition& point,
    bool value)
{
    if (point.type == PointType::Boolean) {
        return value;
    }
    if (point.type != PointType::Enum) {
        Fail(
            "semantic point '" + std::string(pointName) +
            "' must be boolean or enum");
    }

    if (pointName == "power") {
        return std::string(value ? "on" : "off");
    }
    if (pointName == "blinds") {
        return std::string(value ? "enabled" : "disabled");
    }
    if (pointName == "blocked") {
        return std::string(value ? "blocked" : "unblocked");
    }

    Fail("unsupported binary semantic point '" + std::string(pointName) + "'");
}

[[nodiscard]] int DecodeAlarmCode(const PointValue& value)
{
    const auto* number = std::get_if<double>(&value);
    if (number == nullptr || !std::isfinite(*number)) {
        Fail("alarmCode must decode to a finite number");
    }

    const double rounded = std::round(*number);
    if (std::abs(*number - rounded) > 1e-9) {
        Fail("alarmCode must decode to a whole number");
    }

    if (rounded < static_cast<double>(std::numeric_limits<int>::min()) ||
        rounded > static_cast<double>(std::numeric_limits<int>::max())) {
        Fail("alarmCode is outside the common integer range");
    }

    return static_cast<int>(rounded);
}

[[nodiscard]] const char* ControlPointName(DriverControl control) noexcept
{
    switch (control) {
    case DriverControl::Power:
        return "power";
    case DriverControl::Mode:
        return "mode";
    case DriverControl::FanSpeed:
        return "fanSpeed";
    case DriverControl::SetTemperature:
        return "setTemperature";
    case DriverControl::Blinds:
        return "blinds";
    case DriverControl::Blocked:
        return "blocked";
    }
    return "";
}

[[nodiscard]] PointValue CommandPointValue(
    DriverControl control,
    const PointDefinition& point,
    const DriverCommandValue& value)
{
    switch (control) {
    case DriverControl::Power:
    case DriverControl::Blinds:
    case DriverControl::Blocked: {
        const auto* boolean = std::get_if<bool>(&value);
        if (boolean == nullptr) {
            Fail("binary driver command requires a boolean value");
        }
        return EncodeBinarySemantic(ControlPointName(control), point, *boolean);
    }

    case DriverControl::Mode: {
        const auto* mode = std::get_if<HvacMode>(&value);
        if (mode == nullptr) {
            Fail("Mode driver command requires an HVAC mode value");
        }
        return std::string(ModeName(*mode));
    }

    case DriverControl::FanSpeed: {
        const auto* speed = std::get_if<HvacFanSpeed>(&value);
        if (speed == nullptr) {
            Fail("FanSpeed driver command requires an HVAC fan-speed value");
        }
        return std::string(FanSpeedName(*speed));
    }

    case DriverControl::SetTemperature: {
        const auto* number = std::get_if<double>(&value);
        if (number == nullptr || !std::isfinite(*number)) {
            Fail("SetTemperature driver command requires a finite numeric value");
        }
        return *number;
    }
    }

    Fail("unsupported driver control");
}

} // namespace

SemanticConversionError::SemanticConversionError(std::string message)
    : std::runtime_error(std::move(message))
{
}

bool IsSemanticPointEnabled(
    const ModbusProfile& profile,
    std::string_view pointName) noexcept
{
    if (pointName == "power") {
        return profile.capabilities.power;
    }
    if (pointName == "mode") {
        return profile.capabilities.mode;
    }
    if (pointName == "fanSpeed") {
        return profile.capabilities.fanSpeed;
    }
    if (pointName == "setTemperature") {
        return profile.capabilities.setTemperature;
    }
    if (pointName == "roomTemperature") {
        return profile.capabilities.roomTemperature;
    }
    if (pointName == "alarmCode") {
        return profile.capabilities.alarm;
    }
    if (pointName == "blinds") {
        return profile.capabilities.blinds;
    }
    if (pointName == "blocked") {
        return profile.capabilities.blocked;
    }
    return false;
}

void ApplySemanticRead(
    DriverDeviceState& state,
    const ModbusProfile& profile,
    std::string_view pointName,
    std::uint16_t rawValue)
{
    if (!IsSemanticPointEnabled(profile, pointName)) {
        Fail(
            "semantic point '" + std::string(pointName) +
            "' is disabled by profile capabilities");
    }

    const auto& point = RequirePoint(profile, pointName);
    if (!point.read.has_value()) {
        Fail(
            "semantic point '" + std::string(pointName) +
            "' has no read location");
    }

    PointValue decoded;
    try {
        decoded = DecodePointValue(point, rawValue);
    }
    catch (const ValueConversionError& error) {
        Fail(
            "semantic point '" + std::string(pointName) +
            "': " + error.what());
    }

    if (pointName == "power") {
        state.power = DecodeBinarySemantic(pointName, decoded);
        return;
    }
    if (pointName == "mode") {
        const auto* text = std::get_if<std::string>(&decoded);
        if (text == nullptr) {
            Fail("mode must decode through an enum mapping");
        }
        state.mode = ParseMode(*text);
        return;
    }
    if (pointName == "fanSpeed") {
        const auto* text = std::get_if<std::string>(&decoded);
        if (text == nullptr) {
            Fail("fanSpeed must decode through an enum mapping");
        }
        state.fanSpeed = ParseFanSpeed(*text);
        return;
    }
    if (pointName == "setTemperature") {
        const auto* number = std::get_if<double>(&decoded);
        if (number == nullptr) {
            Fail("setTemperature must decode to a number");
        }
        state.setTemperature = *number;
        return;
    }
    if (pointName == "roomTemperature") {
        const auto* number = std::get_if<double>(&decoded);
        if (number == nullptr) {
            Fail("roomTemperature must decode to a number");
        }
        state.roomTemperature = *number;
        return;
    }
    if (pointName == "alarmCode") {
        state.alarmCode = DecodeAlarmCode(decoded);
        return;
    }
    if (pointName == "blinds") {
        state.blinds = DecodeBinarySemantic(pointName, decoded);
        return;
    }
    if (pointName == "blocked") {
        state.blocked = DecodeBinarySemantic(pointName, decoded);
        return;
    }

    Fail("unsupported semantic point '" + std::string(pointName) + "'");
}

EncodedSemanticWrite EncodeSemanticWrite(
    const ModbusProfile& profile,
    DriverControl control,
    const DriverCommandValue& value)
{
    const std::string_view pointName = ControlPointName(control);
    if (pointName.empty()) {
        Fail("unsupported driver control");
    }
    if (!IsSemanticPointEnabled(profile, pointName)) {
        Fail(
            "semantic point '" + std::string(pointName) +
            "' is disabled by profile capabilities");
    }

    const auto& point = RequirePoint(profile, pointName);
    if (!point.write.has_value()) {
        Fail(
            "semantic point '" + std::string(pointName) +
            "' is not writable");
    }

    try {
        return EncodedSemanticWrite{
            .control = control,
            .location = *point.write,
            .rawValue = EncodePointValue(
                point,
                CommandPointValue(control, point, value)),
        };
    }
    catch (const ValueConversionError& error) {
        Fail(
            "semantic point '" + std::string(pointName) +
            "': " + error.what());
    }
}

} // namespace mdv::modbus
