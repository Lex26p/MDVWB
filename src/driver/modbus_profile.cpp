#include "modbus_profile.h"

#include "json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <set>
#include <vector>
#include <utility>

namespace mdv::modbus {
namespace {

using mdvwb::json::Object;
using mdvwb::json::Value;

[[noreturn]] void Fail(std::string message)
{
    throw ProfileError(std::move(message));
}

[[nodiscard]] const Object& RequireObject(
    const Value& value,
    std::string_view path)
{
    if (!value.IsObject()) {
        Fail(std::string(path) + " must be an object");
    }
    return value.AsObject();
}

[[nodiscard]] const Value& RequireField(
    const Object& object,
    std::string_view key,
    std::string_view path)
{
    const auto iterator = object.find(key);
    if (iterator == object.end()) {
        Fail(
            std::string(path) + " is missing required field '" +
            std::string(key) + "'");
    }
    return iterator->second;
}

void RejectUnknownFields(
    const Object& object,
    std::initializer_list<std::string_view> allowed,
    std::string_view path)
{
    for (const auto& [key, unused] : object) {
        static_cast<void>(unused);
        const auto known = std::find(allowed.begin(), allowed.end(), key);
        if (known == allowed.end()) {
            Fail(
                std::string(path) + " contains unknown field '" +
                key + "'");
        }
    }
}

[[nodiscard]] std::int64_t RequireInteger(
    const Value& value,
    std::string_view path)
{
    if (!value.IsInteger()) {
        Fail(std::string(path) + " must be an integer");
    }
    return value.AsInteger();
}

[[nodiscard]] double RequireNumber(
    const Value& value,
    std::string_view path)
{
    if (!value.IsNumber()) {
        Fail(std::string(path) + " must be a number");
    }
    const auto result = value.AsNumber();
    if (!std::isfinite(result)) {
        Fail(std::string(path) + " must be finite");
    }
    return result;
}

[[nodiscard]] bool RequireBoolean(
    const Value& value,
    std::string_view path)
{
    if (!value.IsBoolean()) {
        Fail(std::string(path) + " must be true or false");
    }
    return value.AsBoolean();
}

[[nodiscard]] const std::string& RequireString(
    const Value& value,
    std::string_view path)
{
    if (!value.IsString()) {
        Fail(std::string(path) + " must be a string");
    }
    return value.AsString();
}

[[nodiscard]] std::uint16_t CheckedU16(
    std::int64_t value,
    std::string_view path)
{
    if (value < 0 ||
        value > static_cast<std::int64_t>(
            std::numeric_limits<std::uint16_t>::max())) {
        Fail(std::string(path) + " must be in range 0..65535");
    }
    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] std::uint8_t CheckedLogicalAddress(
    std::int64_t value,
    std::string_view path)
{
    if (value < kMinLogicalAddress || value > kMaxLogicalAddress) {
        Fail(std::string(path) + " must be in range 1..63");
    }
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] std::uint8_t CheckedSlaveId(
    std::int64_t value,
    std::string_view path)
{
    if (value < 1 || value > 247) {
        Fail(std::string(path) + " must be in range 1..247");
    }
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] bool IsValidProfileId(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 64U) {
        return false;
    }

    const auto first = value.front();
    if (first < 'a' || first > 'z') {
        return false;
    }

    return std::all_of(
        value.begin(),
        value.end(),
        [](char character) {
            return
                (character >= 'a' && character <= 'z') ||
                (character >= '0' && character <= '9') ||
                character == '_';
        });
}

[[nodiscard]] bool IsKnownPointName(std::string_view name) noexcept
{
    return
        name == "power" ||
        name == "mode" ||
        name == "fanSpeed" ||
        name == "setTemperature" ||
        name == "roomTemperature" ||
        name == "alarmCode" ||
        name == "blinds" ||
        name == "blocked";
}

[[nodiscard]] SerialParity ParseParity(
    std::string_view value,
    std::string_view path)
{
    if (value == "none") {
        return SerialParity::None;
    }
    if (value == "even") {
        return SerialParity::Even;
    }
    if (value == "odd") {
        return SerialParity::Odd;
    }
    Fail(std::string(path) + " must be one of none, even, odd");
}

[[nodiscard]] RegisterSpace ParseRegisterSpace(
    std::string_view value,
    std::string_view path)
{
    if (value == "coil") {
        return RegisterSpace::Coil;
    }
    if (value == "discrete_input") {
        return RegisterSpace::DiscreteInput;
    }
    if (value == "holding_register") {
        return RegisterSpace::HoldingRegister;
    }
    if (value == "input_register") {
        return RegisterSpace::InputRegister;
    }
    Fail(
        std::string(path) +
        " must be one of coil, discrete_input, holding_register, input_register");
}

[[nodiscard]] PointType ParsePointType(
    std::string_view value,
    std::string_view path)
{
    if (value == "boolean") {
        return PointType::Boolean;
    }
    if (value == "enum") {
        return PointType::Enum;
    }
    if (value == "number") {
        return PointType::Number;
    }
    Fail(std::string(path) + " must be one of boolean, enum, number");
}

[[nodiscard]] WriteRounding ParseRounding(
    std::string_view value,
    std::string_view path)
{
    if (value == "exact") {
        return WriteRounding::Exact;
    }
    if (value == "nearest") {
        return WriteRounding::Nearest;
    }
    if (value == "floor") {
        return WriteRounding::Floor;
    }
    if (value == "ceil") {
        return WriteRounding::Ceil;
    }
    Fail(std::string(path) + " must be one of exact, nearest, floor, ceil");
}

[[nodiscard]] RawType ParseRawType(
    std::string_view value,
    std::string_view path)
{
    if (value == "uint16") {
        return RawType::UInt16;
    }
    if (value == "int16") {
        return RawType::Int16;
    }
    Fail(std::string(path) + " must be one of uint16, int16");
}

[[nodiscard]] RegisterLocation ParseLocation(
    const Value& value,
    std::string_view path,
    bool writable)
{
    const auto& object = RequireObject(value, path);
    RejectUnknownFields(object, {"space", "address", "reference"}, path);

    RegisterLocation result;
    result.space = ParseRegisterSpace(
        RequireString(
            RequireField(object, "space", path),
            std::string(path) + ".space"),
        std::string(path) + ".space");
    result.address = CheckedU16(
        RequireInteger(
            RequireField(object, "address", path),
            std::string(path) + ".address"),
        std::string(path) + ".address");

    if (const auto iterator = object.find("reference");
        iterator != object.end()) {
        const auto& reference = RequireString(
            iterator->second,
            std::string(path) + ".reference");
        if (reference.empty() || reference.size() > 64U) {
            Fail(
                std::string(path) +
                ".reference must contain 1..64 characters");
        }
        result.reference = reference;
    }

    if (writable &&
        (result.space == RegisterSpace::DiscreteInput ||
         result.space == RegisterSpace::InputRegister)) {
        Fail(
            std::string(path) +
            " cannot write to a read-only Modbus data space");
    }

    return result;
}

[[nodiscard]] NumericTransform ParseTransform(
    const Value& value,
    std::string_view path)
{
    const auto& object = RequireObject(value, path);
    RejectUnknownFields(object, {"scale", "offset"}, path);

    NumericTransform result;
    if (const auto iterator = object.find("scale");
        iterator != object.end()) {
        result.scale = RequireNumber(
            iterator->second,
            std::string(path) + ".scale");
    }
    if (const auto iterator = object.find("offset");
        iterator != object.end()) {
        result.offset = RequireNumber(
            iterator->second,
            std::string(path) + ".offset");
    }

    if (result.scale == 0.0) {
        Fail(std::string(path) + ".scale must not be zero");
    }

    return result;
}

[[nodiscard]] NumericLimits ParseLimits(
    const Value& value,
    std::string_view path)
{
    const auto& object = RequireObject(value, path);
    RejectUnknownFields(object, {"min", "max", "step"}, path);

    NumericLimits result;
    if (const auto iterator = object.find("min");
        iterator != object.end()) {
        result.minimum = RequireNumber(
            iterator->second,
            std::string(path) + ".min");
    }
    if (const auto iterator = object.find("max");
        iterator != object.end()) {
        result.maximum = RequireNumber(
            iterator->second,
            std::string(path) + ".max");
    }
    if (const auto iterator = object.find("step");
        iterator != object.end()) {
        result.step = RequireNumber(
            iterator->second,
            std::string(path) + ".step");
    }

    if (result.minimum.has_value() &&
        result.maximum.has_value() &&
        *result.minimum > *result.maximum) {
        Fail(std::string(path) + ".min must not exceed max");
    }
    if (result.step.has_value() && *result.step <= 0.0) {
        Fail(std::string(path) + ".step must be positive");
    }

    return result;
}

[[nodiscard]] std::uint16_t ParseRawMapKey(
    std::string_view text,
    std::string_view path)
{
    unsigned int value = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value, 10);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() ||
        value > std::numeric_limits<std::uint16_t>::max()) {
        Fail(
            std::string(path) +
            " keys must be decimal raw values in range 0..65535");
    }
    return static_cast<std::uint16_t>(value);
}

[[nodiscard]] EnumMappings ParseEnumMappings(
    const Object& point,
    std::string_view path,
    bool readable,
    bool writable)
{
    EnumMappings result;

    if (const auto iterator = point.find("readMap");
        iterator != point.end()) {
        const auto& mapObject = RequireObject(
            iterator->second,
            std::string(path) + ".readMap");
        for (const auto& [rawText, semanticValue] : mapObject) {
            const auto raw = ParseRawMapKey(
                rawText,
                std::string(path) + ".readMap");
            const auto& semantic = RequireString(
                semanticValue,
                std::string(path) + ".readMap." + rawText);
            if (semantic.empty()) {
                Fail(
                    std::string(path) +
                    ".readMap semantic values must not be empty");
            }
            result.read.emplace(raw, semantic);
        }
    }

    if (const auto iterator = point.find("writeMap");
        iterator != point.end()) {
        const auto& mapObject = RequireObject(
            iterator->second,
            std::string(path) + ".writeMap");
        for (const auto& [semantic, rawValue] : mapObject) {
            if (semantic.empty()) {
                Fail(
                    std::string(path) +
                    ".writeMap semantic keys must not be empty");
            }
            result.write.emplace(
                semantic,
                CheckedU16(
                    RequireInteger(
                        rawValue,
                        std::string(path) + ".writeMap." + semantic),
                    std::string(path) + ".writeMap." + semantic));
        }
    }

    if (readable && result.read.empty()) {
        Fail(std::string(path) + ".readMap is required for readable enum points");
    }
    if (writable && result.write.empty()) {
        Fail(std::string(path) + ".writeMap is required for writable enum points");
    }

    return result;
}

[[nodiscard]] PointDefinition ParsePoint(
    const Value& value,
    std::string_view path)
{
    const auto& object = RequireObject(value, path);
    RejectUnknownFields(
        object,
        {
            "type",
            "rawType",
            "read",
            "write",
            "transform",
            "limits",
            "writeConversion",
            "readMap",
            "writeMap",
        },
        path);

    PointDefinition result;
    result.type = ParsePointType(
        RequireString(
            RequireField(object, "type", path),
            std::string(path) + ".type"),
        std::string(path) + ".type");

    if (const auto iterator = object.find("rawType");
        iterator != object.end()) {
        if (result.type != PointType::Number) {
            Fail(
                std::string(path) +
                ".rawType is allowed only for number points");
        }
        result.rawType = ParseRawType(
            RequireString(
                iterator->second,
                std::string(path) + ".rawType"),
            std::string(path) + ".rawType");
    }

    if (const auto iterator = object.find("read");
        iterator != object.end()) {
        result.read = ParseLocation(
            iterator->second,
            std::string(path) + ".read",
            false);
    }
    if (const auto iterator = object.find("write");
        iterator != object.end()) {
        result.write = ParseLocation(
            iterator->second,
            std::string(path) + ".write",
            true);
    }

    if (!result.read.has_value() && !result.write.has_value()) {
        Fail(std::string(path) + " must declare read, write, or both");
    }

    if (const auto iterator = object.find("transform");
        iterator != object.end()) {
        if (result.type != PointType::Number) {
            Fail(
                std::string(path) +
                ".transform is allowed only for number points");
        }
        result.transform = ParseTransform(
            iterator->second,
            std::string(path) + ".transform");
    }

    if (const auto iterator = object.find("limits");
        iterator != object.end()) {
        if (result.type != PointType::Number) {
            Fail(
                std::string(path) +
                ".limits is allowed only for number points");
        }
        result.limits = ParseLimits(
            iterator->second,
            std::string(path) + ".limits");
    }

    if (const auto iterator = object.find("writeConversion");
        iterator != object.end()) {
        if (result.type != PointType::Number || !result.write.has_value()) {
            Fail(
                std::string(path) +
                ".writeConversion requires a writable number point");
        }
        const auto& conversion = RequireObject(
            iterator->second,
            std::string(path) + ".writeConversion");
        RejectUnknownFields(
            conversion,
            {"rounding"},
            std::string(path) + ".writeConversion");
        result.rounding = ParseRounding(
            RequireString(
                RequireField(
                    conversion,
                    "rounding",
                    std::string(path) + ".writeConversion"),
                std::string(path) + ".writeConversion.rounding"),
            std::string(path) + ".writeConversion.rounding");
    }

    if (result.type == PointType::Enum) {
        result.enumMappings = ParseEnumMappings(
            object,
            path,
            result.read.has_value(),
            result.write.has_value());
    }
    else if (object.contains("readMap") || object.contains("writeMap")) {
        Fail(
            std::string(path) +
            ".readMap/writeMap are allowed only for enum points");
    }

    return result;
}

[[nodiscard]] ProfileCapabilities ParseCapabilities(
    const Value& value,
    std::string_view path)
{
    const auto& object = RequireObject(value, path);
    RejectUnknownFields(
        object,
        {
            "power",
            "mode",
            "fanSpeed",
            "setTemperature",
            "roomTemperature",
            "alarm",
            "blinds",
            "blocked",
        },
        path);

    ProfileCapabilities result;

    const auto read = [&](std::string_view name, bool& target) {
        if (const auto iterator = object.find(name);
            iterator != object.end()) {
            target = RequireBoolean(
                iterator->second,
                std::string(path) + "." + std::string(name));
        }
    };

    read("power", result.power);
    read("mode", result.mode);
    read("fanSpeed", result.fanSpeed);
    read("setTemperature", result.setTemperature);
    read("roomTemperature", result.roomTemperature);
    read("alarm", result.alarm);
    read("blinds", result.blinds);
    read("blocked", result.blocked);

    return result;
}

[[nodiscard]] std::pair<std::uint8_t, std::uint8_t> ParseLogicalRange(
    const Object& object,
    std::string_view path)
{
    const auto logicalMin = CheckedLogicalAddress(
        RequireInteger(
            RequireField(object, "logicalMin", path),
            std::string(path) + ".logicalMin"),
        std::string(path) + ".logicalMin");
    const auto logicalMax = CheckedLogicalAddress(
        RequireInteger(
            RequireField(object, "logicalMax", path),
            std::string(path) + ".logicalMax"),
        std::string(path) + ".logicalMax");

    if (logicalMin > logicalMax) {
        Fail(std::string(path) + ".logicalMin must not exceed logicalMax");
    }

    return {logicalMin, logicalMax};
}

[[nodiscard]] std::uint8_t ParseLogicalKey(
    std::string_view text,
    std::string_view path)
{
    unsigned int value = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), value, 10);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
        Fail(std::string(path) + " keys must be decimal logical addresses");
    }
    if (value < kMinLogicalAddress || value > kMaxLogicalAddress) {
        Fail(std::string(path) + " keys must be in range 1..63");
    }
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] Addressing ParseAddressing(
    const Value& value,
    std::string_view path)
{
    const auto& object = RequireObject(value, path);
    const auto& type = RequireString(
        RequireField(object, "type", path),
        std::string(path) + ".type");

    if (type == "direct_slave") {
        RejectUnknownFields(
            object,
            {"type", "logicalMin", "logicalMax", "registerOffset"},
            path);

        const auto [logicalMin, logicalMax] =
            ParseLogicalRange(object, path);

        DirectSlaveAddressing result;
        result.logicalMin = logicalMin;
        result.logicalMax = logicalMax;

        if (const auto iterator = object.find("registerOffset");
            iterator != object.end()) {
            result.registerOffset = CheckedU16(
                RequireInteger(
                    iterator->second,
                    std::string(path) + ".registerOffset"),
                std::string(path) + ".registerOffset");
        }

        if (logicalMax > 247) {
            Fail(
                std::string(path) +
                " direct_slave logical addresses must fit Modbus Slave IDs 1..247");
        }
        return result;
    }

    if (type == "fixed_slave_stride") {
        RejectUnknownFields(
            object,
            {
                "type",
                "logicalMin",
                "logicalMax",
                "slaveId",
                "firstLogicalAddress",
                "registerStride",
            },
            path);

        const auto [logicalMin, logicalMax] =
            ParseLogicalRange(object, path);

        FixedSlaveStrideAddressing result;
        result.logicalMin = logicalMin;
        result.logicalMax = logicalMax;
        result.slaveId = CheckedSlaveId(
            RequireInteger(
                RequireField(object, "slaveId", path),
                std::string(path) + ".slaveId"),
            std::string(path) + ".slaveId");
        result.firstLogicalAddress = CheckedLogicalAddress(
            RequireInteger(
                RequireField(object, "firstLogicalAddress", path),
                std::string(path) + ".firstLogicalAddress"),
            std::string(path) + ".firstLogicalAddress");
        result.registerStride = CheckedU16(
            RequireInteger(
                RequireField(object, "registerStride", path),
                std::string(path) + ".registerStride"),
            std::string(path) + ".registerStride");

        if (result.firstLogicalAddress < logicalMin ||
            result.firstLogicalAddress > logicalMax) {
            Fail(
                std::string(path) +
                ".firstLogicalAddress must be inside logicalMin..logicalMax");
        }

        const auto distance =
            static_cast<std::uint32_t>(logicalMax) -
            static_cast<std::uint32_t>(result.firstLogicalAddress);
        const auto maximumOffset =
            distance * static_cast<std::uint32_t>(result.registerStride);
        if (maximumOffset >
            std::numeric_limits<std::uint16_t>::max()) {
            Fail(
                std::string(path) +
                " registerStride overflows the 16-bit Modbus register range");
        }

        return result;
    }

    if (type == "explicit") {
        RejectUnknownFields(
            object,
            {"type", "logicalMin", "logicalMax", "devices"},
            path);

        const auto [logicalMin, logicalMax] =
            ParseLogicalRange(object, path);

        ExplicitAddressing result;
        result.logicalMin = logicalMin;
        result.logicalMax = logicalMax;

        const auto& devices = RequireObject(
            RequireField(object, "devices", path),
            std::string(path) + ".devices");
        if (devices.empty()) {
            Fail(std::string(path) + ".devices must not be empty");
        }

        for (const auto& [logicalText, locationValue] : devices) {
            const auto logical = ParseLogicalKey(
                logicalText,
                std::string(path) + ".devices");

            if (logical < logicalMin || logical > logicalMax) {
                Fail(
                    std::string(path) +
                    ".devices contains a logical address outside logicalMin..logicalMax");
            }

            const auto locationPath =
                std::string(path) + ".devices." + logicalText;
            const auto& location = RequireObject(
                locationValue,
                locationPath);
            RejectUnknownFields(
                location,
                {"slaveId", "registerOffset"},
                locationPath);

            ExplicitDeviceLocation parsed;
            parsed.slaveId = CheckedSlaveId(
                RequireInteger(
                    RequireField(location, "slaveId", locationPath),
                    locationPath + ".slaveId"),
                locationPath + ".slaveId");
            parsed.registerOffset = CheckedU16(
                RequireInteger(
                    RequireField(location, "registerOffset", locationPath),
                    locationPath + ".registerOffset"),
                locationPath + ".registerOffset");

            result.devices.emplace(logical, parsed);
        }

        return result;
    }

    Fail(
        std::string(path) +
        ".type must be one of direct_slave, fixed_slave_stride, explicit");
}

[[nodiscard]] ProbeDefinition ParseProbe(
    const Value& value,
    std::string_view path)
{
    const auto& object = RequireObject(value, path);
    RejectUnknownFields(object, {"read", "quantity"}, path);

    ProbeDefinition result;
    result.read = ParseLocation(
        RequireField(object, "read", path),
        std::string(path) + ".read",
        false);

    if (const auto iterator = object.find("quantity");
        iterator != object.end()) {
        const auto quantity = RequireInteger(
            iterator->second,
            std::string(path) + ".quantity");
        if (quantity < 1 || quantity > 125) {
            Fail(std::string(path) + ".quantity must be in range 1..125");
        }
        result.quantity = static_cast<std::uint16_t>(quantity);
    }

    const auto lastAddress =
        static_cast<std::uint32_t>(result.read.address) +
        static_cast<std::uint32_t>(result.quantity) - 1U;
    if (lastAddress > std::numeric_limits<std::uint16_t>::max()) {
        Fail(std::string(path) + " register range exceeds 0xFFFF");
    }

    return result;
}

void ValidateCapabilityPoints(const ModbusProfile& profile)
{
    const auto hasPoint = [&](std::string_view name) {
        return profile.points.contains(name);
    };

    const auto require = [&](bool capability, std::string_view point) {
        if (capability && !hasPoint(point)) {
            Fail(
                "capability '" + std::string(point) +
                "' is true but points." + std::string(point) +
                " is missing");
        }
    };

    require(profile.capabilities.power, "power");
    require(profile.capabilities.mode, "mode");
    require(profile.capabilities.fanSpeed, "fanSpeed");
    require(profile.capabilities.setTemperature, "setTemperature");
    require(profile.capabilities.roomTemperature, "roomTemperature");
    require(profile.capabilities.blinds, "blinds");
    require(profile.capabilities.blocked, "blocked");

    if (profile.capabilities.alarm &&
        !profile.points.contains("alarmCode")) {
        Fail(
            "capability 'alarm' is true but points.alarmCode is missing");
    }
}

[[nodiscard]] ModbusProfile Convert(const Value& rootValue)
{
    const auto& root = RequireObject(rootValue, "root");
    RejectUnknownFields(
        root,
        {
            "schemaVersion",
            "id",
            "name",
            "registerAddressing",
            "transport",
            "addressing",
            "capabilities",
            "probe",
            "points",
        },
        "root");

    ModbusProfile result;

    const auto schemaVersion = RequireInteger(
        RequireField(root, "schemaVersion", "root"),
        "root.schemaVersion");
    if (schemaVersion != kProfileSchemaVersion) {
        Fail("root.schemaVersion must be exactly 1");
    }
    result.schemaVersion = static_cast<int>(schemaVersion);

    result.id = RequireString(
        RequireField(root, "id", "root"),
        "root.id");
    if (!IsValidProfileId(result.id)) {
        Fail(
            "root.id must start with a-z, contain only a-z, 0-9 and _, and be at most 64 characters");
    }

    result.name = RequireString(
        RequireField(root, "name", "root"),
        "root.name");
    if (result.name.empty() || result.name.size() > 128U) {
        Fail("root.name must contain 1..128 characters");
    }

    result.registerAddressing = RequireString(
        RequireField(root, "registerAddressing", "root"),
        "root.registerAddressing");
    if (result.registerAddressing != "pdu_zero_based") {
        Fail("root.registerAddressing must be 'pdu_zero_based'");
    }

    const auto& transport = RequireObject(
        RequireField(root, "transport", "root"),
        "root.transport");
    RejectUnknownFields(
        transport,
        {"baudRate", "dataBits", "parity", "stopBits"},
        "root.transport");

    result.transport.baudRate = static_cast<std::uint32_t>(
        RequireInteger(
            RequireField(transport, "baudRate", "root.transport"),
            "root.transport.baudRate"));
    result.transport.dataBits = static_cast<std::uint8_t>(
        RequireInteger(
            RequireField(transport, "dataBits", "root.transport"),
            "root.transport.dataBits"));
    result.transport.parity = ParseParity(
        RequireString(
            RequireField(transport, "parity", "root.transport"),
            "root.transport.parity"),
        "root.transport.parity");
    result.transport.stopBits = static_cast<std::uint8_t>(
        RequireInteger(
            RequireField(transport, "stopBits", "root.transport"),
            "root.transport.stopBits"));

    try {
        ValidateSerialSettings(result.transport);
    }
    catch (const std::exception& error) {
        Fail(std::string("root.transport: ") + error.what());
    }

    result.addressing = ParseAddressing(
        RequireField(root, "addressing", "root"),
        "root.addressing");

    result.capabilities = ParseCapabilities(
        RequireField(root, "capabilities", "root"),
        "root.capabilities");

    result.probe = ParseProbe(
        RequireField(root, "probe", "root"),
        "root.probe");

    const auto& points = RequireObject(
        RequireField(root, "points", "root"),
        "root.points");
    if (points.empty()) {
        Fail("root.points must not be empty");
    }

    for (const auto& [name, pointValue] : points) {
        if (!IsKnownPointName(name)) {
            Fail("root.points contains unsupported semantic point '" + name + "'");
        }
        result.points.emplace(
            name,
            ParsePoint(
                pointValue,
                "root.points." + name));
    }

    ValidateCapabilityPoints(result);
    return result;
}

} // namespace

ProfileError::ProfileError(std::string message)
    : std::runtime_error(std::move(message))
{
}

ModbusProfile ParseProfile(std::string_view jsonText)
{
    try {
        return Convert(mdvwb::json::Parse(jsonText));
    }
    catch (const mdvwb::json::ParseError& error) {
        throw ProfileError(error.what());
    }
}

ModbusProfile LoadProfileFile(const std::filesystem::path& path)
{
    try {
        return Convert(mdvwb::json::ParseFile(path));
    }
    catch (const mdvwb::json::ParseError& error) {
        throw ProfileError(error.what());
    }
    catch (const ProfileError&) {
        throw;
    }
    catch (const std::exception& error) {
        throw ProfileError(
            "cannot load Modbus profile '" + path.string() +
            "': " + error.what());
    }
}

const ModbusProfile* ProfileCatalog::Find(std::string_view id) const noexcept
{
    const auto iterator = profiles.find(id);
    return iterator == profiles.end() ? nullptr : &iterator->second;
}

bool ProfileCatalog::HasErrors() const noexcept
{
    return !issues.empty();
}

ProfileCatalog LoadProfileDirectory(
    const std::filesystem::path& directory)
{
    std::error_code error;

    const bool exists = std::filesystem::exists(directory, error);
    if (error) {
        throw ProfileError(
            "cannot inspect Modbus profile directory '" +
            directory.string() + "': " + error.message());
    }
    if (!exists) {
        throw ProfileError(
            "Modbus profile directory does not exist: '" +
            directory.string() + "'");
    }

    const bool isDirectory = std::filesystem::is_directory(directory, error);
    if (error) {
        throw ProfileError(
            "cannot inspect Modbus profile directory '" +
            directory.string() + "': " + error.message());
    }
    if (!isDirectory) {
        throw ProfileError(
            "Modbus profile path is not a directory: '" +
            directory.string() + "'");
    }

    std::vector<std::filesystem::path> files;
    std::filesystem::directory_iterator iterator(directory, error);
    if (error) {
        throw ProfileError(
            "cannot enumerate Modbus profile directory '" +
            directory.string() + "': " + error.message());
    }

    const std::filesystem::directory_iterator end;
    while (iterator != end) {
        const auto path = iterator->path();

        std::error_code typeError;
        const bool regular = iterator->is_regular_file(typeError);
        if (typeError) {
            throw ProfileError(
                "cannot inspect Modbus profile candidate '" +
                path.string() + "': " + typeError.message());
        }

        if (regular && path.extension() == ".json") {
            files.push_back(path);
        }

        iterator.increment(error);
        if (error) {
            throw ProfileError(
                "cannot enumerate Modbus profile directory '" +
                directory.string() + "': " + error.message());
        }
    }

    std::sort(
        files.begin(),
        files.end(),
        [](const auto& left, const auto& right) {
            return left.generic_string() < right.generic_string();
        });

    struct Candidate {
        std::filesystem::path path;
        ModbusProfile profile;
    };

    std::vector<Candidate> candidates;
    ProfileCatalog result;

    for (const auto& path : files) {
        try {
            candidates.push_back(
                Candidate{
                    .path = path,
                    .profile = LoadProfileFile(path),
                });
        }
        catch (const ProfileError& profileError) {
            result.issues.push_back(
                ProfileLoadIssue{
                    .path = path,
                    .error = profileError.what(),
                });
        }
    }

    std::map<std::string, std::vector<std::size_t>, std::less<>> byId;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        byId[candidates[index].profile.id].push_back(index);
    }

    for (const auto& [id, indices] : byId) {
        if (indices.size() == 1U) {
            auto& candidate = candidates[indices.front()];
            result.profiles.emplace(
                candidate.profile.id,
                std::move(candidate.profile));
            continue;
        }

        for (const auto index : indices) {
            result.issues.push_back(
                ProfileLoadIssue{
                    .path = candidates[index].path,
                    .error =
                        "duplicate Modbus profile id '" + id +
                        "'; every profile with this id was rejected",
                });
        }
    }

    std::sort(
        result.issues.begin(),
        result.issues.end(),
        [](const ProfileLoadIssue& left, const ProfileLoadIssue& right) {
            const auto leftPath = left.path.generic_string();
            const auto rightPath = right.path.generic_string();
            if (leftPath != rightPath) {
                return leftPath < rightPath;
            }
            return left.error < right.error;
        });

    return result;
}

} // namespace mdv::modbus
