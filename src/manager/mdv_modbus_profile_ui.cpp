#include "mdv_modbus_profile_ui.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <map>
#include <optional>
#include <ostream>
#include <sstream>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace mdvwb {
namespace {

struct CapabilityDescriptor {
    std::string_view jsonName;
    std::string_view pointName;
    bool mdv::modbus::ProfileCapabilities::*enabled;
};

constexpr std::array<CapabilityDescriptor, 8> kCapabilities{{
    {"power", "power", &mdv::modbus::ProfileCapabilities::power},
    {"mode", "mode", &mdv::modbus::ProfileCapabilities::mode},
    {"fanSpeed", "fanSpeed", &mdv::modbus::ProfileCapabilities::fanSpeed},
    {"setTemperature", "setTemperature", &mdv::modbus::ProfileCapabilities::setTemperature},
    {"roomTemperature", "roomTemperature", &mdv::modbus::ProfileCapabilities::roomTemperature},
    {"alarm", "alarmCode", &mdv::modbus::ProfileCapabilities::alarm},
    {"blinds", "blinds", &mdv::modbus::ProfileCapabilities::blinds},
    {"blocked", "blocked", &mdv::modbus::ProfileCapabilities::blocked},
}};

[[nodiscard]] std::string JsonEscape(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 8U);
    constexpr char Hex[] = "0123456789abcdef";

    for (const unsigned char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20U) {
                result += "\\u00";
                result.push_back(Hex[(character >> 4U) & 0x0fU]);
                result.push_back(Hex[character & 0x0fU]);
            }
            else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return result;
}

void WriteString(std::ostream& output, std::string_view value)
{
    output << '"' << JsonEscape(value) << '"';
}

[[nodiscard]] std::string_view ParityName(mdv::SerialParity parity)
{
    switch (parity) {
    case mdv::SerialParity::None: return "none";
    case mdv::SerialParity::Even: return "even";
    case mdv::SerialParity::Odd: return "odd";
    }
    return "none";
}

[[nodiscard]] std::string_view PointTypeName(mdv::modbus::PointType type)
{
    switch (type) {
    case mdv::modbus::PointType::Boolean: return "boolean";
    case mdv::modbus::PointType::Enum: return "enum";
    case mdv::modbus::PointType::Number: return "number";
    }
    return "boolean";
}

[[nodiscard]] std::pair<int, int> LogicalRange(
    const mdv::modbus::Addressing& addressing)
{
    return std::visit(
        [](const auto& value) {
            return std::pair<int, int>{
                static_cast<int>(value.logicalMin),
                static_cast<int>(value.logicalMax),
            };
        },
        addressing);
}

[[nodiscard]] std::string_view AddressingTypeName(
    const mdv::modbus::Addressing& addressing)
{
    if (std::holds_alternative<mdv::modbus::DirectSlaveAddressing>(addressing)) {
        return "direct_slave";
    }
    if (std::holds_alternative<mdv::modbus::FixedSlaveStrideAddressing>(addressing)) {
        return "fixed_slave_stride";
    }
    return "explicit";
}

void WriteOptionalNumber(
    std::ostream& output,
    std::string_view name,
    const std::optional<double>& value)
{
    output << ',';
    WriteString(output, name);
    output << ':';
    if (!value.has_value() || !std::isfinite(*value)) {
        output << "null";
        return;
    }
    output << std::setprecision(15) << *value;
}

void WriteEnumValues(
    std::ostream& output,
    const mdv::modbus::PointDefinition& point)
{
    std::map<std::string, std::pair<bool, bool>, std::less<>> values;
    for (const auto& [raw, semantic] : point.enumMappings.read) {
        static_cast<void>(raw);
        values[semantic].first = true;
    }
    for (const auto& [semantic, raw] : point.enumMappings.write) {
        static_cast<void>(raw);
        values[semantic].second = true;
    }

    output << ",\"values\":[";
    bool first = true;
    for (const auto& [semantic, access] : values) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << '{';
        output << "\"value\":";
        WriteString(output, semantic);
        output << ",\"readable\":" << (access.first ? "true" : "false");
        output << ",\"writable\":" << (access.second ? "true" : "false");
        output << '}';
    }
    output << ']';
}

void WriteCapability(
    std::ostream& output,
    const mdv::modbus::ModbusProfile& profile,
    const CapabilityDescriptor& descriptor)
{
    const bool supported = profile.capabilities.*(descriptor.enabled);
    const auto pointIterator = profile.points.find(descriptor.pointName);
    const mdv::modbus::PointDefinition* point =
        pointIterator == profile.points.end() ? nullptr : &pointIterator->second;

    WriteString(output, descriptor.jsonName);
    output << ":{";
    output << "\"supported\":" << (supported ? "true" : "false");
    output << ",\"readable\":"
           << (supported && point != nullptr && point->read.has_value()
                   ? "true" : "false");
    output << ",\"writable\":"
           << (supported && point != nullptr && point->write.has_value()
                   ? "true" : "false");

    if (point != nullptr) {
        output << ",\"type\":";
        WriteString(output, PointTypeName(point->type));

        if (point->type == mdv::modbus::PointType::Enum) {
            WriteEnumValues(output, *point);
        }
        if (point->type == mdv::modbus::PointType::Number) {
            const mdv::modbus::NumericLimits empty;
            const auto& limits = point->limits.has_value() ? *point->limits : empty;
            WriteOptionalNumber(output, "minimum", limits.minimum);
            WriteOptionalNumber(output, "maximum", limits.maximum);
            WriteOptionalNumber(output, "step", limits.step);
        }
    }
    else {
        output << ",\"type\":null";
    }

    output << '}';
}

void WriteProfile(
    std::ostream& output,
    const mdv::modbus::ModbusProfile& profile)
{
    const auto [logicalMinimum, logicalMaximum] = LogicalRange(profile.addressing);

    output << '{';
    output << "\"id\":";
    WriteString(output, profile.id);
    output << ",\"name\":";
    WriteString(output, profile.name);
    output << ",\"transport\":{";
    output << "\"baudRate\":" << profile.transport.baudRate;
    output << ",\"dataBits\":" << static_cast<int>(profile.transport.dataBits);
    output << ",\"parity\":";
    WriteString(output, ParityName(profile.transport.parity));
    output << ",\"stopBits\":" << static_cast<int>(profile.transport.stopBits);
    output << '}';
    output << ",\"logicalAddresses\":{";
    output << "\"minimum\":" << logicalMinimum;
    output << ",\"maximum\":" << logicalMaximum;
    output << '}';
    output << ",\"addressingType\":";
    WriteString(output, AddressingTypeName(profile.addressing));
    output << ",\"capabilities\":{";

    for (std::size_t index = 0; index < kCapabilities.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        WriteCapability(output, profile, kCapabilities[index]);
    }

    output << "}}";
}

} // namespace

std::string SerializeModbusProfileUiCatalog(
    const mdv::modbus::ProfileCatalog& catalog)
{
    std::ostringstream output;
    output << '{';
    output << "\"schemaVersion\":" << kModbusProfileUiSchemaVersion;
    output << ",\"profiles\":[";

    bool firstProfile = true;
    for (const auto& [id, profile] : catalog.profiles) {
        static_cast<void>(id);
        if (!firstProfile) {
            output << ',';
        }
        firstProfile = false;
        WriteProfile(output, profile);
    }
    output << ']';

    std::vector<mdv::modbus::ProfileLoadIssue> issues = catalog.issues;
    std::sort(
        issues.begin(),
        issues.end(),
        [](const auto& left, const auto& right) {
            // Sort by the same sanitized values that are exposed to the UI.
            // Ordering by the original server path is platform-dependent and
            // may disagree with the emitted basename.
            const auto leftFile = left.path.filename().generic_string();
            const auto rightFile = right.path.filename().generic_string();
            return std::tie(leftFile, left.error) <
                std::tie(rightFile, right.error);
        });

    output << ",\"issues\":[";
    for (std::size_t index = 0; index < issues.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << '{';
        output << "\"file\":";
        WriteString(output, issues[index].path.filename().string());
        output << ",\"message\":";
        WriteString(output, issues[index].error);
        output << '}';
    }
    output << "]}";
    return output.str();
}

std::string LoadModbusProfileUiCatalog(
    const std::filesystem::path& directory)
{
    return SerializeModbusProfileUiCatalog(
        mdv::modbus::LoadProfileDirectory(directory));
}

} // namespace mdvwb
