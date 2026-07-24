#include "mdv_buses_config.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <variant>

namespace mdvwb {
namespace {

struct JsonValue;
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

struct JsonValue {
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, std::string, JsonArray, JsonObject>;
    Storage value;
};

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    JsonValue Parse() {
        SkipWhitespace();
        JsonValue result = ParseValue();
        SkipWhitespace();
        if (!AtEnd()) {
            Fail("unexpected characters after the root value");
        }
        return result;
    }

private:
    JsonValue ParseValue() {
        if (AtEnd()) {
            Fail("unexpected end of JSON");
        }

        switch (Peek()) {
            case '{':
                return JsonValue{ParseObject()};
            case '[':
                return JsonValue{ParseArray()};
            case '"':
                return JsonValue{ParseString()};
            case 't':
                ConsumeLiteral("true");
                return JsonValue{true};
            case 'f':
                ConsumeLiteral("false");
                return JsonValue{false};
            case 'n':
                ConsumeLiteral("null");
                return JsonValue{nullptr};
            default:
                if (Peek() == '-' || std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                    return JsonValue{ParseInteger()};
                }
                Fail("expected an object, array, string, integer, boolean or null");
        }
    }

    JsonObject ParseObject() {
        Expect('{');
        SkipWhitespace();

        JsonObject object;
        if (TryConsume('}')) {
            return object;
        }

        while (true) {
            if (Peek() != '"') {
                Fail("expected an object key");
            }
            const std::string key = ParseString();
            SkipWhitespace();
            Expect(':');
            SkipWhitespace();

            if (object.find(key) != object.end()) {
                Fail("duplicate object key '" + key + "'");
            }
            object.emplace(key, ParseValue());
            SkipWhitespace();

            if (TryConsume('}')) {
                break;
            }
            Expect(',');
            SkipWhitespace();
        }
        return object;
    }

    JsonArray ParseArray() {
        Expect('[');
        SkipWhitespace();

        JsonArray array;
        if (TryConsume(']')) {
            return array;
        }

        while (true) {
            array.push_back(ParseValue());
            SkipWhitespace();
            if (TryConsume(']')) {
                break;
            }
            Expect(',');
            SkipWhitespace();
        }
        return array;
    }

    std::string ParseString() {
        Expect('"');
        std::string result;

        while (!AtEnd()) {
            const char ch = Consume();
            if (ch == '"') {
                return result;
            }
            if (static_cast<unsigned char>(ch) < 0x20U) {
                Fail("control character inside a string");
            }
            if (ch != '\\') {
                result.push_back(ch);
                continue;
            }

            if (AtEnd()) {
                Fail("unfinished string escape");
            }
            const char escaped = Consume();
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                case 'u':
                    AppendUnicodeEscape(result);
                    break;
                default:
                    Fail("invalid string escape");
            }
        }
        Fail("unterminated string");
    }

    void AppendUnicodeEscape(std::string& result) {
        unsigned int codePoint = 0;
        for (int index = 0; index < 4; ++index) {
            if (AtEnd()) {
                Fail("unfinished unicode escape");
            }
            const char ch = Consume();
            codePoint <<= 4U;
            if (ch >= '0' && ch <= '9') {
                codePoint += static_cast<unsigned int>(ch - '0');
            } else if (ch >= 'a' && ch <= 'f') {
                codePoint += static_cast<unsigned int>(ch - 'a' + 10);
            } else if (ch >= 'A' && ch <= 'F') {
                codePoint += static_cast<unsigned int>(ch - 'A' + 10);
            } else {
                Fail("invalid unicode escape");
            }
        }

        if (codePoint <= 0x7FU) {
            result.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7FFU) {
            result.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            result.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        } else {
            result.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            result.push_back(static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            result.push_back(static_cast<char>(0x80U | (codePoint & 0x3FU)));
        }
    }

    std::int64_t ParseInteger() {
        const std::size_t begin = position_;
        if (Peek() == '-') {
            ++position_;
        }
        if (AtEnd()) {
            Fail("unfinished integer");
        }

        if (Peek() == '0') {
            ++position_;
            if (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                Fail("leading zero in integer");
            }
        } else {
            if (std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
                Fail("invalid integer");
            }
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                ++position_;
            }
        }

        if (!AtEnd() && (Peek() == '.' || Peek() == 'e' || Peek() == 'E')) {
            Fail("only integer numbers are supported in buses.json");
        }

        const std::string token(text_.substr(begin, position_ - begin));
        try {
            std::size_t consumed = 0;
            const long long value = std::stoll(token, &consumed, 10);
            if (consumed != token.size()) {
                Fail("invalid integer");
            }
            return static_cast<std::int64_t>(value);
        } catch (const std::exception&) {
            Fail("integer is outside the supported range");
        }
    }

    void ConsumeLiteral(std::string_view literal) {
        for (const char expected : literal) {
            if (AtEnd() || Consume() != expected) {
                Fail("invalid literal");
            }
        }
    }

    void SkipWhitespace() {
        while (!AtEnd()) {
            const char ch = Peek();
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
                break;
            }
            ++position_;
        }
    }

    void Expect(char expected) {
        if (AtEnd() || Consume() != expected) {
            Fail(std::string("expected '") + expected + "'");
        }
    }

    bool TryConsume(char expected) {
        if (!AtEnd() && Peek() == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    char Peek() const {
        return text_[position_];
    }

    char Consume() {
        return text_[position_++];
    }

    bool AtEnd() const {
        return position_ >= text_.size();
    }

    [[noreturn]] void Fail(const std::string& message) const {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t index = 0; index < position_ && index < text_.size(); ++index) {
            if (text_[index] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        throw BusesConfigError(
            "JSON error at line " + std::to_string(line) +
            ", column " + std::to_string(column) + ": " + message);
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

const JsonObject& RequireObject(const JsonValue& value, std::string_view path) {
    const auto* object = std::get_if<JsonObject>(&value.value);
    if (object == nullptr) {
        throw BusesConfigError(std::string(path) + " must be an object");
    }
    return *object;
}

const JsonArray& RequireArray(const JsonValue& value, std::string_view path) {
    const auto* array = std::get_if<JsonArray>(&value.value);
    if (array == nullptr) {
        throw BusesConfigError(std::string(path) + " must be an array");
    }
    return *array;
}

std::int64_t RequireInteger(const JsonValue& value, std::string_view path) {
    const auto* integer = std::get_if<std::int64_t>(&value.value);
    if (integer == nullptr) {
        throw BusesConfigError(std::string(path) + " must be an integer");
    }
    return *integer;
}

bool RequireBoolean(const JsonValue& value, std::string_view path) {
    const auto* boolean = std::get_if<bool>(&value.value);
    if (boolean == nullptr) {
        throw BusesConfigError(std::string(path) + " must be true or false");
    }
    return *boolean;
}

const std::string& RequireString(const JsonValue& value, std::string_view path) {
    const auto* string = std::get_if<std::string>(&value.value);
    if (string == nullptr) {
        throw BusesConfigError(std::string(path) + " must be a string");
    }
    return *string;
}

const JsonValue& RequireField(const JsonObject& object, std::string_view key, std::string_view path) {
    const auto iterator = object.find(std::string(key));
    if (iterator == object.end()) {
        throw BusesConfigError(std::string(path) + " is missing required field '" + std::string(key) + "'");
    }
    return iterator->second;
}

void RejectUnknownFields(
    const JsonObject& object,
    const std::set<std::string>& allowed,
    std::string_view path) {
    for (const auto& [key, unused] : object) {
        static_cast<void>(unused);
        if (allowed.find(key) == allowed.end()) {
            throw BusesConfigError(std::string(path) + " contains unknown field '" + key + "'");
        }
    }
}

int CheckedInt(std::int64_t value, int minimum, int maximum, std::string_view path) {
    if (value < minimum || value > maximum) {
        throw BusesConfigError(
            std::string(path) + " must be in range " + std::to_string(minimum) + ".." + std::to_string(maximum));
    }
    return static_cast<int>(value);
}

bool IsValidDevicePath(std::string_view port) {
    if (!port.starts_with("/dev/") || port.size() <= 5U) {
        return false;
    }
    for (const char ch : port) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (std::isalnum(byte) != 0 || ch == '/' || ch == '_' || ch == '-' ||
            ch == '.' || ch == '+' || ch == ':') {
            continue;
        }
        return false;
    }
    return true;
}

std::string EscapeJson(std::string_view value) {
    std::ostringstream output;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (ch < 0x20U) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned int>(ch) << std::dec;
                } else {
                    output << static_cast<char>(ch);
                }
        }
    }
    return output.str();
}

BusesConfig ValidateAndConvert(const JsonValue& rootValue) {
    const JsonObject& root = RequireObject(rootValue, "root");
    RejectUnknownFields(root, {"version", "buses"}, "root");

    BusesConfig result;
    result.version = CheckedInt(
        RequireInteger(RequireField(root, "version", "root"), "root.version"),
        1,
        1,
        "root.version");

    const JsonArray& buses = RequireArray(RequireField(root, "buses", "root"), "root.buses");
    std::set<int> usedIds;
    std::set<std::string> usedPorts;

    for (std::size_t index = 0; index < buses.size(); ++index) {
        const std::string path = "root.buses[" + std::to_string(index) + "]";
        const JsonObject& object = RequireObject(buses[index], path);
        RejectUnknownFields(object, {"id", "enabled", "port", "addresses"}, path);

        BusConfig bus;
        bus.id = CheckedInt(
            RequireInteger(RequireField(object, "id", path), path + ".id"),
            1,
            999,
            path + ".id");
        bus.enabled = RequireBoolean(RequireField(object, "enabled", path), path + ".enabled");
        bus.port = RequireString(RequireField(object, "port", path), path + ".port");

        if (!IsValidDevicePath(bus.port)) {
            throw BusesConfigError(
                path + ".port must be a safe absolute device path beginning with /dev/");
        }
        if (!usedIds.insert(bus.id).second) {
            throw BusesConfigError("duplicate bus id " + std::to_string(bus.id));
        }
        if (!usedPorts.insert(bus.port).second) {
            throw BusesConfigError("device port '" + bus.port + "' is assigned to more than one bus");
        }

        const JsonArray& addresses = RequireArray(
            RequireField(object, "addresses", path),
            path + ".addresses");
        std::set<int> uniqueAddresses;
        for (std::size_t addressIndex = 0; addressIndex < addresses.size(); ++addressIndex) {
            const std::string addressPath =
                path + ".addresses[" + std::to_string(addressIndex) + "]";
            const int address = CheckedInt(
                RequireInteger(addresses[addressIndex], addressPath),
                0,
                63,
                addressPath);
            if (!uniqueAddresses.insert(address).second) {
                throw BusesConfigError(
                    path + ".addresses contains duplicate address " + std::to_string(address));
            }
            bus.addresses.push_back(address);
        }

        if (bus.enabled && bus.addresses.empty()) {
            throw BusesConfigError(path + " is enabled but has no polling addresses");
        }

        std::sort(bus.addresses.begin(), bus.addresses.end());
        result.buses.push_back(std::move(bus));
    }

    std::sort(result.buses.begin(), result.buses.end(), [](const BusConfig& left, const BusConfig& right) {
        return left.id < right.id;
    });
    return result;
}

}  // namespace

BusesConfig ParseBusesConfig(std::string_view jsonText) {
    return ValidateAndConvert(JsonParser(jsonText).Parse());
}

BusesConfig LoadBusesConfig(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw BusesConfigError("cannot open buses configuration file: " + path.string());
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw BusesConfigError("cannot read buses configuration file: " + path.string());
    }
    return ParseBusesConfig(buffer.str());
}

std::string SerializeBusesConfig(const BusesConfig& config) {
    BusesConfig normalized = config;
    if (normalized.version != 1) {
        throw BusesConfigError("configuration version must be 1");
    }

    std::set<int> usedIds;
    std::set<std::string> usedPorts;
    for (BusConfig& bus : normalized.buses) {
        if (bus.id < 1 || bus.id > 999) {
            throw BusesConfigError("bus id must be in range 1..999");
        }
        if (!IsValidDevicePath(bus.port)) {
            throw BusesConfigError("bus " + std::to_string(bus.id) + " has an invalid device port");
        }
        if (!usedIds.insert(bus.id).second) {
            throw BusesConfigError("duplicate bus id " + std::to_string(bus.id));
        }
        if (!usedPorts.insert(bus.port).second) {
            throw BusesConfigError("device port '" + bus.port + "' is assigned to more than one bus");
        }

        std::sort(bus.addresses.begin(), bus.addresses.end());
        const auto duplicate = std::adjacent_find(bus.addresses.begin(), bus.addresses.end());
        if (duplicate != bus.addresses.end()) {
            throw BusesConfigError(
                "bus " + std::to_string(bus.id) + " contains duplicate address " +
                std::to_string(*duplicate));
        }
        for (const int address : bus.addresses) {
            if (address < 0 || address > 63) {
                throw BusesConfigError(
                    "bus " + std::to_string(bus.id) + " address must be in range 0..63");
            }
        }
        if (bus.enabled && bus.addresses.empty()) {
            throw BusesConfigError(
                "bus " + std::to_string(bus.id) + " is enabled but has no polling addresses");
        }
    }

    std::sort(normalized.buses.begin(), normalized.buses.end(),
              [](const BusConfig& left, const BusConfig& right) { return left.id < right.id; });

    std::ostringstream output;
    output << "{\n  \"version\": 1,\n  \"buses\": [";
    if (!normalized.buses.empty()) {
        output << '\n';
    }

    for (std::size_t index = 0; index < normalized.buses.size(); ++index) {
        const BusConfig& bus = normalized.buses[index];
        output << "    {\n"
               << "      \"id\": " << bus.id << ",\n"
               << "      \"enabled\": " << (bus.enabled ? "true" : "false") << ",\n"
               << "      \"port\": \"" << EscapeJson(bus.port) << "\",\n"
               << "      \"addresses\": [";
        for (std::size_t addressIndex = 0; addressIndex < bus.addresses.size(); ++addressIndex) {
            if (addressIndex != 0) {
                output << ", ";
            }
            output << bus.addresses[addressIndex];
        }
        output << "]\n    }";
        if (index + 1 != normalized.buses.size()) {
            output << ',';
        }
        output << '\n';
    }

    output << "  ]\n}\n";
    return output.str();
}

}  // namespace mdvwb
