#include "mdv_buses_config.h"

#include "json.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>

namespace mdvwb {
namespace {

using mdvwb::json::Array;
using mdvwb::json::Object;
using mdvwb::json::Value;

[[noreturn]] void Fail(std::string message)
{
    throw BusesConfigError(std::move(message));
}

const Object& RequireObject(
    const Value& value,
    std::string_view path)
{
    if (!value.IsObject()) {
        Fail(std::string(path) + " must be an object");
    }
    return value.AsObject();
}

const Array& RequireArray(
    const Value& value,
    std::string_view path)
{
    if (!value.IsArray()) {
        Fail(std::string(path) + " must be an array");
    }
    return value.AsArray();
}

std::int64_t RequireInteger(
    const Value& value,
    std::string_view path)
{
    if (!value.IsInteger()) {
        Fail(std::string(path) + " must be an integer");
    }
    return value.AsInteger();
}

bool RequireBoolean(
    const Value& value,
    std::string_view path)
{
    if (!value.IsBoolean()) {
        Fail(std::string(path) + " must be true or false");
    }
    return value.AsBoolean();
}

const std::string& RequireString(
    const Value& value,
    std::string_view path)
{
    if (!value.IsString()) {
        Fail(std::string(path) + " must be a string");
    }
    return value.AsString();
}

const Value& RequireField(
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
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            Fail(
                std::string(path) + " contains unknown field '" +
                key + "'");
        }
    }
}

int CheckedInt(
    std::int64_t value,
    int minimum,
    int maximum,
    std::string_view path)
{
    if (value < minimum || value > maximum) {
        Fail(
            std::string(path) + " must be in range " +
            std::to_string(minimum) + ".." +
            std::to_string(maximum));
    }
    return static_cast<int>(value);
}

bool IsValidDevicePath(std::string_view port)
{
    if (!port.starts_with("/dev/") || port.size() <= 5U) {
        return false;
    }

    return std::all_of(
        port.begin(),
        port.end(),
        [](char character) {
            const unsigned char byte =
                static_cast<unsigned char>(character);
            return
                std::isalnum(byte) != 0 ||
                character == '/' ||
                character == '_' ||
                character == '-' ||
                character == '.' ||
                character == '+' ||
                character == ':';
        });
}

bool IsValidProfileId(std::string_view value) noexcept
{
    if (value.empty() || value.size() > 64U) {
        return false;
    }
    if (value.front() < 'a' || value.front() > 'z') {
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

bool IsSupportedBaudRate(int baudRate) noexcept
{
    constexpr std::array<int, 8> supported{
        1200,
        2400,
        4800,
        9600,
        19200,
        38400,
        57600,
        115200,
    };

    return std::find(
        supported.begin(),
        supported.end(),
        baudRate) != supported.end();
}

BusProtocol ParseProtocol(
    std::string_view value,
    std::string_view path)
{
    if (value == "mdv") {
        return BusProtocol::Mdv;
    }
    if (value == "modbus_rtu") {
        return BusProtocol::ModbusRtu;
    }

    Fail(
        std::string(path) +
        " must be one of mdv, modbus_rtu");
}

BusParity ParseParity(
    std::string_view value,
    std::string_view path)
{
    if (value == "none") {
        return BusParity::None;
    }
    if (value == "even") {
        return BusParity::Even;
    }
    if (value == "odd") {
        return BusParity::Odd;
    }

    Fail(
        std::string(path) +
        " must be one of none, even, odd");
}

void ValidateModbusSettings(
    const ModbusBusSettings& settings,
    std::string_view path)
{
    if (!IsValidProfileId(settings.profileId)) {
        Fail(
            std::string(path) +
            ".profileId must start with a-z, contain only a-z, 0-9 and _, and be at most 64 characters");
    }

    if (!IsSupportedBaudRate(settings.baudRate)) {
        Fail(
            std::string(path) +
            ".baudRate must be one of 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200");
    }

    if (settings.dataBits != 7 && settings.dataBits != 8) {
        Fail(
            std::string(path) +
            ".dataBits must be 7 or 8");
    }

    if (settings.stopBits != 1 && settings.stopBits != 2) {
        Fail(
            std::string(path) +
            ".stopBits must be 1 or 2");
    }
}

ModbusBusSettings ParseModbusSettings(
    const Value& value,
    std::string_view path)
{
    const auto& object = RequireObject(value, path);
    RejectUnknownFields(
        object,
        {
            "profileId",
            "baudRate",
            "dataBits",
            "parity",
            "stopBits",
        },
        path);

    ModbusBusSettings result;
    result.profileId = RequireString(
        RequireField(object, "profileId", path),
        std::string(path) + ".profileId");
    result.baudRate = CheckedInt(
        RequireInteger(
            RequireField(object, "baudRate", path),
            std::string(path) + ".baudRate"),
        1,
        std::numeric_limits<int>::max(),
        std::string(path) + ".baudRate");
    result.dataBits = CheckedInt(
        RequireInteger(
            RequireField(object, "dataBits", path),
            std::string(path) + ".dataBits"),
        1,
        8,
        std::string(path) + ".dataBits");
    result.parity = ParseParity(
        RequireString(
            RequireField(object, "parity", path),
            std::string(path) + ".parity"),
        std::string(path) + ".parity");
    result.stopBits = CheckedInt(
        RequireInteger(
            RequireField(object, "stopBits", path),
            std::string(path) + ".stopBits"),
        1,
        2,
        std::string(path) + ".stopBits");

    ValidateModbusSettings(result, path);
    return result;
}

int MinimumAddress(BusProtocol protocol) noexcept
{
    return protocol == BusProtocol::ModbusRtu ? 1 : 0;
}

void ValidateBus(
    BusConfig& bus,
    std::set<int>& usedIds,
    std::set<std::string>& usedPorts,
    std::string_view path)
{
    if (bus.id < 1 || bus.id > 999) {
        Fail(std::string(path) + ".id must be in range 1..999");
    }

    if (!IsValidDevicePath(bus.port)) {
        Fail(
            std::string(path) +
            ".port must be a safe absolute device path beginning with /dev/");
    }

    if (!usedIds.insert(bus.id).second) {
        Fail("duplicate bus id " + std::to_string(bus.id));
    }

    if (!usedPorts.insert(bus.port).second) {
        Fail(
            "device port '" + bus.port +
            "' is assigned to more than one bus");
    }

    if (bus.protocol == BusProtocol::Mdv) {
        if (bus.modbus.has_value()) {
            Fail(
                std::string(path) +
                ".modbus is allowed only when protocol is modbus_rtu");
        }
    }
    else {
        if (!bus.modbus.has_value()) {
            Fail(
                std::string(path) +
                " uses protocol modbus_rtu but is missing required field 'modbus'");
        }
        ValidateModbusSettings(
            *bus.modbus,
            std::string(path) + ".modbus");
    }

    std::sort(bus.addresses.begin(), bus.addresses.end());
    const auto duplicate =
        std::adjacent_find(
            bus.addresses.begin(),
            bus.addresses.end());
    if (duplicate != bus.addresses.end()) {
        Fail(
            std::string(path) +
            ".addresses contains duplicate address " +
            std::to_string(*duplicate));
    }

    const int minimum = MinimumAddress(bus.protocol);
    for (const int address : bus.addresses) {
        if (address < minimum || address > 63) {
            Fail(
                std::string(path) +
                ".addresses address must be in range " +
                std::to_string(minimum) +
                "..63 for protocol " +
                std::string(BusProtocolName(bus.protocol)));
        }
    }

    if (bus.enabled && bus.addresses.empty()) {
        Fail(
            std::string(path) +
            " is enabled but has no polling addresses");
    }
}

std::string EscapeJson(std::string_view value)
{
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U) {
                output << '?';
            }
            else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    return output.str();
}

BusesConfig ValidateAndConvert(const Value& rootValue)
{
    const auto& root = RequireObject(rootValue, "root");
    RejectUnknownFields(
        root,
        {"version", "revision", "buses"},
        "root");

    BusesConfig result;
    result.version = CheckedInt(
        RequireInteger(
            RequireField(root, "version", "root"),
            "root.version"),
        1,
        1,
        "root.version");

    if (const auto iterator = root.find("revision");
        iterator != root.end()) {
        result.revision = CheckedInt(
            RequireInteger(
                iterator->second,
                "root.revision"),
            0,
            std::numeric_limits<int>::max(),
            "root.revision");
    }

    const auto& buses = RequireArray(
        RequireField(root, "buses", "root"),
        "root.buses");

    std::set<int> usedIds;
    std::set<std::string> usedPorts;

    for (std::size_t index = 0;
         index < buses.size();
         ++index) {
        const std::string path =
            "root.buses[" + std::to_string(index) + "]";
        const auto& object =
            RequireObject(buses[index], path);

        RejectUnknownFields(
            object,
            {
                "id",
                "enabled",
                "protocol",
                "port",
                "addresses",
                "modbus",
            },
            path);

        BusConfig bus;
        bus.id = CheckedInt(
            RequireInteger(
                RequireField(object, "id", path),
                path + ".id"),
            1,
            999,
            path + ".id");
        bus.enabled = RequireBoolean(
            RequireField(object, "enabled", path),
            path + ".enabled");
        bus.port = RequireString(
            RequireField(object, "port", path),
            path + ".port");

        // Compatibility contract: protocol omitted means existing MDV.
        if (const auto iterator = object.find("protocol");
            iterator != object.end()) {
            bus.protocol = ParseProtocol(
                RequireString(
                    iterator->second,
                    path + ".protocol"),
                path + ".protocol");
        }

        if (const auto iterator = object.find("modbus");
            iterator != object.end()) {
            bus.modbus = ParseModbusSettings(
                iterator->second,
                path + ".modbus");
        }

        const auto& addresses = RequireArray(
            RequireField(object, "addresses", path),
            path + ".addresses");

        bus.addresses.reserve(addresses.size());
        for (std::size_t addressIndex = 0;
             addressIndex < addresses.size();
             ++addressIndex) {
            const std::string addressPath =
                path + ".addresses[" +
                std::to_string(addressIndex) + "]";
            bus.addresses.push_back(
                CheckedInt(
                    RequireInteger(
                        addresses[addressIndex],
                        addressPath),
                    0,
                    63,
                    addressPath));
        }

        ValidateBus(
            bus,
            usedIds,
            usedPorts,
            path);
        result.buses.push_back(std::move(bus));
    }

    std::sort(
        result.buses.begin(),
        result.buses.end(),
        [](const BusConfig& left, const BusConfig& right) {
            return left.id < right.id;
        });

    return result;
}

}  // namespace

std::string_view BusProtocolName(BusProtocol protocol) noexcept
{
    switch (protocol) {
    case BusProtocol::Mdv:
        return "mdv";
    case BusProtocol::ModbusRtu:
        return "modbus_rtu";
    }
    return "unknown";
}

std::string_view BusParityName(BusParity parity) noexcept
{
    switch (parity) {
    case BusParity::None:
        return "none";
    case BusParity::Even:
        return "even";
    case BusParity::Odd:
        return "odd";
    }
    return "unknown";
}

BusesConfig ParseBusesConfig(std::string_view jsonText)
{
    try {
        return ValidateAndConvert(json::Parse(jsonText));
    }
    catch (const json::ParseError& error) {
        throw BusesConfigError(error.what());
    }
}

BusesConfig LoadBusesConfig(
    const std::filesystem::path& path)
{
    try {
        return ValidateAndConvert(json::ParseFile(path));
    }
    catch (const json::ParseError& error) {
        throw BusesConfigError(error.what());
    }
    catch (const BusesConfigError&) {
        throw;
    }
    catch (const std::exception& error) {
        throw BusesConfigError(
            "cannot load buses configuration '" +
            path.string() + "': " + error.what());
    }
}

std::string SerializeBusesConfig(
    const BusesConfig& config)
{
    BusesConfig normalized = config;

    if (normalized.version != 1) {
        throw BusesConfigError(
            "configuration version must be 1");
    }
    if (normalized.revision < 0) {
        throw BusesConfigError(
            "configuration revision must be in range 0..2147483647");
    }

    std::set<int> usedIds;
    std::set<std::string> usedPorts;
    for (std::size_t index = 0;
         index < normalized.buses.size();
         ++index) {
        ValidateBus(
            normalized.buses[index],
            usedIds,
            usedPorts,
            "root.buses[" + std::to_string(index) + "]");
    }

    std::sort(
        normalized.buses.begin(),
        normalized.buses.end(),
        [](const BusConfig& left, const BusConfig& right) {
            return left.id < right.id;
        });

    std::ostringstream output;
    output
        << "{\n"
        << "  \"version\": 1,\n"
        << "  \"revision\": "
        << normalized.revision << ",\n"
        << "  \"buses\": [";

    if (!normalized.buses.empty()) {
        output << '\n';
    }

    for (std::size_t index = 0;
         index < normalized.buses.size();
         ++index) {
        const auto& bus = normalized.buses[index];

        output
            << "    {\n"
            << "      \"id\": " << bus.id << ",\n"
            << "      \"enabled\": "
            << (bus.enabled ? "true" : "false")
            << ",\n"
            << "      \"protocol\": \""
            << BusProtocolName(bus.protocol)
            << "\",\n"
            << "      \"port\": \""
            << EscapeJson(bus.port)
            << "\",\n";

        if (bus.protocol == BusProtocol::ModbusRtu) {
            const auto& modbus = *bus.modbus;
            output
                << "      \"modbus\": {\n"
                << "        \"profileId\": \""
                << EscapeJson(modbus.profileId)
                << "\",\n"
                << "        \"baudRate\": "
                << modbus.baudRate << ",\n"
                << "        \"dataBits\": "
                << modbus.dataBits << ",\n"
                << "        \"parity\": \""
                << BusParityName(modbus.parity)
                << "\",\n"
                << "        \"stopBits\": "
                << modbus.stopBits << "\n"
                << "      },\n";
        }

        output << "      \"addresses\": [";
        for (std::size_t addressIndex = 0;
             addressIndex < bus.addresses.size();
             ++addressIndex) {
            if (addressIndex != 0U) {
                output << ", ";
            }
            output << bus.addresses[addressIndex];
        }
        output << "]\n    }";

        if (index + 1U != normalized.buses.size()) {
            output << ',';
        }
        output << '\n';
    }

    output << "  ]\n}\n";
    return output.str();
}

}  // namespace mdvwb
