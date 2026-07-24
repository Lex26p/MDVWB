#include "mdv_dashboard_config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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
    using Storage = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;
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
            case '{': return JsonValue{ParseObject()};
            case '[': return JsonValue{ParseArray()};
            case '"': return JsonValue{ParseString()};
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
                    return JsonValue{ParseNumber()};
                }
                Fail("expected an object, array, string, number, boolean or null");
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
            if (AtEnd() || Peek() != '"') {
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
                case 'u': AppendUnicodeEscape(result); break;
                default: Fail("invalid string escape");
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

        if (codePoint >= 0xD800U && codePoint <= 0xDFFFU) {
            Fail("UTF-16 surrogate escapes are not supported");
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

    double ParseNumber() {
        const std::size_t begin = position_;
        if (Peek() == '-') {
            ++position_;
        }
        if (AtEnd()) {
            Fail("unfinished number");
        }

        if (Peek() == '0') {
            ++position_;
            if (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                Fail("leading zero in number");
            }
        } else {
            if (std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
                Fail("invalid number");
            }
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                ++position_;
            }
        }

        if (!AtEnd() && Peek() == '.') {
            ++position_;
            if (AtEnd() || std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
                Fail("fraction requires at least one digit");
            }
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                ++position_;
            }
        }

        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            ++position_;
            if (!AtEnd() && (Peek() == '+' || Peek() == '-')) {
                ++position_;
            }
            if (AtEnd() || std::isdigit(static_cast<unsigned char>(Peek())) == 0) {
                Fail("exponent requires at least one digit");
            }
            while (!AtEnd() && std::isdigit(static_cast<unsigned char>(Peek())) != 0) {
                ++position_;
            }
        }

        const std::string token(text_.substr(begin, position_ - begin));
        try {
            std::size_t consumed = 0;
            const double value = std::stod(token, &consumed);
            if (consumed != token.size() || !std::isfinite(value)) {
                Fail("number is outside the supported range");
            }
            return value;
        } catch (const std::exception&) {
            Fail("number is outside the supported range");
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

    char Peek() const { return text_[position_]; }
    char Consume() { return text_[position_++]; }
    bool AtEnd() const { return position_ >= text_.size(); }

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
        throw DashboardConfigError(
            "JSON error at line " + std::to_string(line) +
            ", column " + std::to_string(column) + ": " + message);
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

const JsonObject& RequireObject(const JsonValue& value, std::string_view path) {
    const auto* object = std::get_if<JsonObject>(&value.value);
    if (object == nullptr) {
        throw DashboardConfigError(std::string(path) + " must be an object");
    }
    return *object;
}

const JsonArray& RequireArray(const JsonValue& value, std::string_view path) {
    const auto* array = std::get_if<JsonArray>(&value.value);
    if (array == nullptr) {
        throw DashboardConfigError(std::string(path) + " must be an array");
    }
    return *array;
}

const std::string& RequireString(const JsonValue& value, std::string_view path) {
    const auto* string = std::get_if<std::string>(&value.value);
    if (string == nullptr) {
        throw DashboardConfigError(std::string(path) + " must be a string");
    }
    return *string;
}

bool RequireBoolean(const JsonValue& value, std::string_view path) {
    const auto* boolean = std::get_if<bool>(&value.value);
    if (boolean == nullptr) {
        throw DashboardConfigError(std::string(path) + " must be true or false");
    }
    return *boolean;
}

double RequireNumber(const JsonValue& value, std::string_view path) {
    const auto* number = std::get_if<double>(&value.value);
    if (number == nullptr || !std::isfinite(*number)) {
        throw DashboardConfigError(std::string(path) + " must be a finite number");
    }
    return *number;
}

const JsonValue& RequireField(const JsonObject& object, std::string_view key, std::string_view path) {
    const auto iterator = object.find(std::string(key));
    if (iterator == object.end()) {
        throw DashboardConfigError(
            std::string(path) + " is missing required field '" + std::string(key) + "'");
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
            throw DashboardConfigError(std::string(path) + " contains unknown field '" + key + "'");
        }
    }
}

int RequireIntegerInRange(
    const JsonValue& value,
    int minimum,
    int maximum,
    std::string_view path) {
    const double number = RequireNumber(value, path);
    if (std::floor(number) != number) {
        throw DashboardConfigError(std::string(path) + " must be an integer");
    }
    if (number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
        throw DashboardConfigError(
            std::string(path) + " must be in range " + std::to_string(minimum) +
            ".." + std::to_string(maximum));
    }
    return static_cast<int>(number);
}

double RequireNumberInRange(
    const JsonValue& value,
    double minimum,
    double maximum,
    std::string_view path) {
    const double number = RequireNumber(value, path);
    if (number < minimum || number > maximum) {
        std::ostringstream message;
        message << path << " must be in range " << minimum << ".." << maximum;
        throw DashboardConfigError(message.str());
    }
    return number;
}

void RequireLength(
    const std::string& value,
    std::size_t minimum,
    std::size_t maximum,
    std::string_view path) {
    if (value.size() < minimum || value.size() > maximum) {
        throw DashboardConfigError(
            std::string(path) + " length must be in range " + std::to_string(minimum) +
            ".." + std::to_string(maximum) + " bytes");
    }
}

bool IsSafeIdentifier(std::string_view value) {
    if (value.empty()) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        return std::isalnum(byte) != 0 || ch == '_' || ch == '-';
    });
}

bool IsSafeAssetFile(std::string_view value) {
    if (value.empty()) {
        return true;
    }
    if (value == "." || value == "..") {
        return false;
    }
    if (!std::all_of(value.begin(), value.end(), [](char ch) {
            const unsigned char byte = static_cast<unsigned char>(ch);
            return std::isalnum(byte) != 0 || ch == '_' || ch == '-' || ch == '.';
        })) {
        return false;
    }

    const std::size_t dot = value.find_last_of('.');
    if (dot == std::string_view::npos) {
        return false;
    }
    std::string extension(value.substr(dot + 1));
    std::transform(extension.begin(), extension.end(), extension.begin(), [](char ch) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    });
    return extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "webp";
}

std::string EscapeJson(std::string_view value) {
    std::ostringstream output;
    for (const unsigned char byte : value) {
        switch (byte) {
            case '"': output << "\\\""; break;
            case '\\': output << "\\\\"; break;
            case '\b': output << "\\b"; break;
            case '\f': output << "\\f"; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (byte < 0x20U) {
                    output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned int>(byte) << std::dec << std::setfill(' ');
                } else {
                    output << static_cast<char>(byte);
                }
        }
    }
    return output.str();
}

std::string FormatNumber(double value) {
    if (value == 0.0) {
        return "0";
    }
    std::ostringstream output;
    output << std::setprecision(12) << value;
    std::string text = output.str();
    const std::size_t exponent = text.find_first_of("eE");
    const std::size_t dot = text.find('.');
    if (dot != std::string::npos && exponent == std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text;
}

DashboardBackground ParseBackground(const JsonValue& value) {
    const JsonObject& object = RequireObject(value, "dashboard.background");
    RejectUnknownFields(
        object,
        {"file", "naturalWidth", "naturalHeight", "defaultScale", "fit"},
        "dashboard.background");

    DashboardBackground result;
    result.file = RequireString(RequireField(object, "file", "dashboard.background"),
                                "dashboard.background.file");
    result.naturalWidth = RequireIntegerInRange(
        RequireField(object, "naturalWidth", "dashboard.background"),
        0, 8192, "dashboard.background.naturalWidth");
    result.naturalHeight = RequireIntegerInRange(
        RequireField(object, "naturalHeight", "dashboard.background"),
        0, 8192, "dashboard.background.naturalHeight");
    result.defaultScale = RequireNumberInRange(
        RequireField(object, "defaultScale", "dashboard.background"),
        0.25, 4.0, "dashboard.background.defaultScale");
    result.fit = RequireString(RequireField(object, "fit", "dashboard.background"),
                               "dashboard.background.fit");

    if (!IsSafeAssetFile(result.file)) {
        throw DashboardConfigError(
            "dashboard.background.file must be empty or a safe PNG/JPEG/WebP file name");
    }
    if (result.file.empty()) {
        if (result.naturalWidth != 0 || result.naturalHeight != 0) {
            throw DashboardConfigError(
                "dashboard.background dimensions must be zero when no image is configured");
        }
    } else if (result.naturalWidth == 0 || result.naturalHeight == 0) {
        throw DashboardConfigError(
            "dashboard.background dimensions must be positive when an image is configured");
    }
    if (result.fit != "contain" && result.fit != "width" &&
        result.fit != "actual" && result.fit != "custom") {
        throw DashboardConfigError(
            "dashboard.background.fit must be contain, width, actual or custom");
    }
    return result;
}

DashboardFanPlacement ParsePlacement(const JsonValue& value, std::size_t index) {
    const std::string path = "dashboard.fans[" + std::to_string(index) + "]";
    const JsonObject& object = RequireObject(value, path);
    RejectUnknownFields(
        object,
        {"id", "number", "bus", "address", "label", "x", "y", "markerScale", "rotation", "visible"},
        path);

    DashboardFanPlacement result;
    result.id = RequireString(RequireField(object, "id", path), path + ".id");
    const auto number = object.find("number");
    result.number = number == object.end()
        ? static_cast<int>(index + 1U)
        : RequireIntegerInRange(number->second, 1, 200, path + ".number");
    result.bus = RequireIntegerInRange(RequireField(object, "bus", path), 1, 999, path + ".bus");
    result.address = RequireIntegerInRange(
        RequireField(object, "address", path), 0, 63, path + ".address");
    result.label = RequireString(RequireField(object, "label", path), path + ".label");
    result.x = RequireNumberInRange(RequireField(object, "x", path), 0.0, 1.0, path + ".x");
    result.y = RequireNumberInRange(RequireField(object, "y", path), 0.0, 1.0, path + ".y");
    result.markerScale = RequireNumberInRange(
        RequireField(object, "markerScale", path), 0.5, 3.0, path + ".markerScale");
    result.rotation = RequireNumberInRange(
        RequireField(object, "rotation", path), -180.0, 180.0, path + ".rotation");
    result.visible = RequireBoolean(RequireField(object, "visible", path), path + ".visible");

    RequireLength(result.id, 1, 64, path + ".id");
    RequireLength(result.label, 1, 120, path + ".label");
    if (!IsSafeIdentifier(result.id)) {
        throw DashboardConfigError(path + ".id may contain only letters, digits, '_' and '-'");
    }
    return result;
}

void ValidateConfig(const DashboardConfig& config) {
    if (config.version != 1) {
        throw DashboardConfigError("dashboard.version must be exactly 1");
    }
    if (config.revision < 0) {
        throw DashboardConfigError("dashboard.revision must not be negative");
    }
    RequireLength(config.title, 1, 160, "dashboard.title");
    if (config.fans.size() > 4096U) {
        throw DashboardConfigError("dashboard.fans may contain at most 4096 placements");
    }

    if (!IsSafeAssetFile(config.background.file)) {
        throw DashboardConfigError(
            "dashboard.background.file must be empty or a safe PNG/JPEG/WebP file name");
    }
    if (config.background.file.empty()) {
        if (config.background.naturalWidth != 0 || config.background.naturalHeight != 0) {
            throw DashboardConfigError(
                "dashboard.background dimensions must be zero when no image is configured");
        }
    } else if (config.background.naturalWidth < 1 || config.background.naturalWidth > 8192 ||
               config.background.naturalHeight < 1 || config.background.naturalHeight > 8192) {
        throw DashboardConfigError(
            "dashboard.background dimensions must be in range 1..8192 when an image is configured");
    }
    if (!std::isfinite(config.background.defaultScale) ||
        config.background.defaultScale < 0.25 || config.background.defaultScale > 4.0) {
        throw DashboardConfigError("dashboard.background.defaultScale must be in range 0.25..4");
    }
    if (config.background.fit != "contain" && config.background.fit != "width" &&
        config.background.fit != "actual" && config.background.fit != "custom") {
        throw DashboardConfigError(
            "dashboard.background.fit must be contain, width, actual or custom");
    }

    std::set<std::string> ids;
    std::set<int> numbers;
    std::set<std::pair<int, int>> devices;
    for (const DashboardFanPlacement& fan : config.fans) {
        RequireLength(fan.id, 1, 64, "dashboard fan id");
        RequireLength(fan.label, 1, 120, "dashboard fan label");
        if (!IsSafeIdentifier(fan.id)) {
            throw DashboardConfigError(
                "dashboard fan id may contain only letters, digits, '_' and '-'");
        }
        if (fan.number < 1 || fan.number > 200) {
            throw DashboardConfigError("dashboard fan number must be in range 1..200");
        }
        if (fan.bus < 1 || fan.bus > 999) {
            throw DashboardConfigError("dashboard fan bus must be in range 1..999");
        }
        if (fan.address < 0 || fan.address > 63) {
            throw DashboardConfigError("dashboard fan address must be in range 0..63");
        }
        if (!std::isfinite(fan.x) || fan.x < 0.0 || fan.x > 1.0 ||
            !std::isfinite(fan.y) || fan.y < 0.0 || fan.y > 1.0) {
            throw DashboardConfigError("dashboard fan coordinates must be in range 0..1");
        }
        if (!std::isfinite(fan.markerScale) || fan.markerScale < 0.5 || fan.markerScale > 3.0) {
            throw DashboardConfigError("dashboard fan markerScale must be in range 0.5..3");
        }
        if (!std::isfinite(fan.rotation) || fan.rotation < -180.0 || fan.rotation > 180.0) {
            throw DashboardConfigError("dashboard fan rotation must be in range -180..180");
        }
        if (!ids.insert(fan.id).second) {
            throw DashboardConfigError("dashboard.fans contains duplicate id '" + fan.id + "'");
        }
        if (!numbers.insert(fan.number).second) {
            throw DashboardConfigError(
                "dashboard.fans contains duplicate number " + std::to_string(fan.number));
        }
        if (!devices.emplace(fan.bus, fan.address).second) {
            throw DashboardConfigError(
                "dashboard.fans contains duplicate device Fan-" + std::to_string(fan.bus) +
                "_" + std::to_string(fan.address));
        }
    }
}

}  // namespace

DashboardConfig ParseDashboardConfig(std::string_view jsonText) {
    const JsonValue rootValue = JsonParser(jsonText).Parse();
    const JsonObject& root = RequireObject(rootValue, "dashboard");
    RejectUnknownFields(root, {"version", "revision", "title", "background", "fans"}, "dashboard");

    DashboardConfig result;
    result.version = RequireIntegerInRange(RequireField(root, "version", "dashboard"),
                                           1, 1, "dashboard.version");
    result.revision = RequireIntegerInRange(RequireField(root, "revision", "dashboard"),
                                            0, std::numeric_limits<int>::max(),
                                            "dashboard.revision");
    result.title = RequireString(RequireField(root, "title", "dashboard"), "dashboard.title");
    result.background = ParseBackground(RequireField(root, "background", "dashboard"));

    const JsonArray& fans = RequireArray(RequireField(root, "fans", "dashboard"), "dashboard.fans");
    if (fans.size() > 4096U) {
        throw DashboardConfigError("dashboard.fans may contain at most 4096 placements");
    }
    result.fans.reserve(fans.size());
    for (std::size_t index = 0; index < fans.size(); ++index) {
        result.fans.push_back(ParsePlacement(fans[index], index));
    }

    ValidateConfig(result);
    return result;
}

DashboardConfig LoadDashboardConfig(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw DashboardConfigError("cannot open dashboard configuration: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw DashboardConfigError("cannot read dashboard configuration: " + path.string());
    }
    return ParseDashboardConfig(buffer.str());
}

std::string SerializeDashboardConfig(const DashboardConfig& config) {
    ValidateConfig(config);

    std::vector<DashboardFanPlacement> fans = config.fans;
    std::sort(fans.begin(), fans.end(), [](const DashboardFanPlacement& left,
                                          const DashboardFanPlacement& right) {
        if (left.bus != right.bus) {
            return left.bus < right.bus;
        }
        if (left.address != right.address) {
            return left.address < right.address;
        }
        return left.id < right.id;
    });

    std::ostringstream output;
    output << "{\n"
           << "  \"version\": " << config.version << ",\n"
           << "  \"revision\": " << config.revision << ",\n"
           << "  \"title\": \"" << EscapeJson(config.title) << "\",\n"
           << "  \"background\": {\n"
           << "    \"file\": \"" << EscapeJson(config.background.file) << "\",\n"
           << "    \"naturalWidth\": " << config.background.naturalWidth << ",\n"
           << "    \"naturalHeight\": " << config.background.naturalHeight << ",\n"
           << "    \"defaultScale\": " << FormatNumber(config.background.defaultScale) << ",\n"
           << "    \"fit\": \"" << EscapeJson(config.background.fit) << "\"\n"
           << "  },\n"
           << "  \"fans\": [";

    if (!fans.empty()) {
        output << '\n';
    }
    for (std::size_t index = 0; index < fans.size(); ++index) {
        const DashboardFanPlacement& fan = fans[index];
        output << "    {\n"
               << "      \"id\": \"" << EscapeJson(fan.id) << "\",\n"
               << "      \"number\": " << fan.number << ",\n"
               << "      \"bus\": " << fan.bus << ",\n"
               << "      \"address\": " << fan.address << ",\n"
               << "      \"label\": \"" << EscapeJson(fan.label) << "\",\n"
               << "      \"x\": " << FormatNumber(fan.x) << ",\n"
               << "      \"y\": " << FormatNumber(fan.y) << ",\n"
               << "      \"markerScale\": " << FormatNumber(fan.markerScale) << ",\n"
               << "      \"rotation\": " << FormatNumber(fan.rotation) << ",\n"
               << "      \"visible\": " << (fan.visible ? "true" : "false") << '\n'
               << "    }";
        if (index + 1U != fans.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

std::vector<DashboardReferenceIssue> InspectDashboardReferences(
    const DashboardConfig& dashboard,
    const BusesConfig& buses) {
    std::map<int, std::set<int>> addressesByBus;
    for (const BusConfig& bus : buses.buses) {
        addressesByBus[bus.id] = std::set<int>(bus.addresses.begin(), bus.addresses.end());
    }

    std::vector<DashboardReferenceIssue> issues;
    for (const DashboardFanPlacement& fan : dashboard.fans) {
        const auto bus = addressesByBus.find(fan.bus);
        if (bus == addressesByBus.end()) {
            issues.push_back({DashboardReferenceIssueKind::MissingBus,
                              fan.id, fan.bus, fan.address, ""});
            continue;
        }
        if (bus->second.find(fan.address) == bus->second.end()) {
            issues.push_back({DashboardReferenceIssueKind::MissingAddress,
                              fan.id, fan.bus, fan.address, ""});
        }
    }
    return issues;
}


namespace {

DashboardPanel ParseCollectionPanel(const JsonValue& value, std::size_t index) {
    const std::string path = "dashboard.panels[" + std::to_string(index) + "]";
    const JsonObject& object = RequireObject(value, path);
    RejectUnknownFields(object, {"id", "title", "background", "fans"}, path);

    DashboardPanel panel;
    panel.id = RequireString(RequireField(object, "id", path), path + ".id");
    panel.title = RequireString(RequireField(object, "title", path), path + ".title");
    panel.background = ParseBackground(RequireField(object, "background", path));
    RequireLength(panel.id, 1, 48, path + ".id");
    RequireLength(panel.title, 1, 160, path + ".title");
    if (!IsSafeIdentifier(panel.id)) {
        throw DashboardConfigError(path + ".id may contain only letters, digits, '_' and '-'");
    }

    const JsonArray& fans = RequireArray(RequireField(object, "fans", path), path + ".fans");
    if (fans.size() > 4096U) {
        throw DashboardConfigError(path + ".fans may contain at most 4096 placements");
    }
    panel.fans.reserve(fans.size());
    for (std::size_t fanIndex = 0; fanIndex < fans.size(); ++fanIndex) {
        panel.fans.push_back(ParsePlacement(fans[fanIndex], fanIndex));
    }

    DashboardConfig compatibility;
    compatibility.title = panel.title;
    compatibility.background = panel.background;
    compatibility.fans = panel.fans;
    ValidateConfig(compatibility);
    return panel;
}

void ValidateDashboardCollection(const DashboardCollection& collection) {
    if (collection.version != 2) {
        throw DashboardConfigError("dashboard.version must be exactly 2");
    }
    if (collection.revision < 0) {
        throw DashboardConfigError("dashboard.revision must not be negative");
    }
    if (collection.panels.empty() || collection.panels.size() > 64U) {
        throw DashboardConfigError("dashboard.panels must contain 1..64 panels");
    }
    RequireLength(collection.defaultPanel, 1, 48, "dashboard.defaultPanel");
    if (!IsSafeIdentifier(collection.defaultPanel)) {
        throw DashboardConfigError(
            "dashboard.defaultPanel may contain only letters, digits, '_' and '-'");
    }

    std::set<std::string> ids;
    bool defaultFound = false;
    for (const DashboardPanel& panel : collection.panels) {
        RequireLength(panel.id, 1, 48, "dashboard panel id");
        RequireLength(panel.title, 1, 160, "dashboard panel title");
        if (!IsSafeIdentifier(panel.id)) {
            throw DashboardConfigError(
                "dashboard panel id may contain only letters, digits, '_' and '-'");
        }
        if (!ids.insert(panel.id).second) {
            throw DashboardConfigError("dashboard.panels contains duplicate id '" + panel.id + "'");
        }
        defaultFound = defaultFound || panel.id == collection.defaultPanel;

        DashboardConfig compatibility;
        compatibility.title = panel.title;
        compatibility.background = panel.background;
        compatibility.fans = panel.fans;
        ValidateConfig(compatibility);
    }
    if (!defaultFound) {
        throw DashboardConfigError("dashboard.defaultPanel does not reference an existing panel");
    }
}

void SerializePanel(std::ostringstream& output, const DashboardPanel& panel, std::string_view indent) {
    std::vector<DashboardFanPlacement> fans = panel.fans;
    std::sort(fans.begin(), fans.end(), [](const DashboardFanPlacement& left,
                                          const DashboardFanPlacement& right) {
        if (left.bus != right.bus) {
            return left.bus < right.bus;
        }
        if (left.address != right.address) {
            return left.address < right.address;
        }
        return left.id < right.id;
    });

    output << indent << "{\n"
           << indent << "  \"id\": \"" << EscapeJson(panel.id) << "\",\n"
           << indent << "  \"title\": \"" << EscapeJson(panel.title) << "\",\n"
           << indent << "  \"background\": {\n"
           << indent << "    \"file\": \"" << EscapeJson(panel.background.file) << "\",\n"
           << indent << "    \"naturalWidth\": " << panel.background.naturalWidth << ",\n"
           << indent << "    \"naturalHeight\": " << panel.background.naturalHeight << ",\n"
           << indent << "    \"defaultScale\": " << FormatNumber(panel.background.defaultScale) << ",\n"
           << indent << "    \"fit\": \"" << EscapeJson(panel.background.fit) << "\"\n"
           << indent << "  },\n"
           << indent << "  \"fans\": [";
    if (!fans.empty()) {
        output << '\n';
    }
    for (std::size_t index = 0; index < fans.size(); ++index) {
        const DashboardFanPlacement& fan = fans[index];
        output << indent << "    {\n"
               << indent << "      \"id\": \"" << EscapeJson(fan.id) << "\",\n"
               << indent << "      \"number\": " << fan.number << ",\n"
               << indent << "      \"bus\": " << fan.bus << ",\n"
               << indent << "      \"address\": " << fan.address << ",\n"
               << indent << "      \"label\": \"" << EscapeJson(fan.label) << "\",\n"
               << indent << "      \"x\": " << FormatNumber(fan.x) << ",\n"
               << indent << "      \"y\": " << FormatNumber(fan.y) << ",\n"
               << indent << "      \"markerScale\": " << FormatNumber(fan.markerScale) << ",\n"
               << indent << "      \"rotation\": " << FormatNumber(fan.rotation) << ",\n"
               << indent << "      \"visible\": " << (fan.visible ? "true" : "false") << '\n'
               << indent << "    }";
        if (index + 1U != fans.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << indent << "  ]\n" << indent << '}';
}

}  // namespace

DashboardCollection ParseDashboardCollection(std::string_view jsonText) {
    const JsonValue rootValue = JsonParser(jsonText).Parse();
    const JsonObject& root = RequireObject(rootValue, "dashboard");
    const int version = RequireIntegerInRange(
        RequireField(root, "version", "dashboard"), 1, 2, "dashboard.version");

    if (version == 1) {
        const DashboardConfig legacy = ParseDashboardConfig(jsonText);
        DashboardCollection migrated;
        migrated.revision = legacy.revision;
        migrated.defaultPanel = "main";
        migrated.panels.clear();
        DashboardPanel panel;
        panel.id = "main";
        panel.title = legacy.title;
        panel.background = legacy.background;
        panel.fans = legacy.fans;
        migrated.panels.push_back(std::move(panel));
        ValidateDashboardCollection(migrated);
        return migrated;
    }

    RejectUnknownFields(
        root, {"version", "revision", "defaultPanel", "panels"}, "dashboard");
    DashboardCollection collection;
    collection.version = 2;
    collection.revision = RequireIntegerInRange(
        RequireField(root, "revision", "dashboard"), 0,
        std::numeric_limits<int>::max(), "dashboard.revision");
    collection.defaultPanel = RequireString(
        RequireField(root, "defaultPanel", "dashboard"), "dashboard.defaultPanel");
    const JsonArray& panels = RequireArray(
        RequireField(root, "panels", "dashboard"), "dashboard.panels");
    if (panels.empty() || panels.size() > 64U) {
        throw DashboardConfigError("dashboard.panels must contain 1..64 panels");
    }
    collection.panels.clear();
    collection.panels.reserve(panels.size());
    for (std::size_t index = 0; index < panels.size(); ++index) {
        collection.panels.push_back(ParseCollectionPanel(panels[index], index));
    }
    ValidateDashboardCollection(collection);
    return collection;
}

DashboardCollection LoadDashboardCollection(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw DashboardConfigError("cannot open dashboard configuration: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw DashboardConfigError("cannot read dashboard configuration: " + path.string());
    }
    return ParseDashboardCollection(buffer.str());
}

std::string SerializeDashboardCollection(const DashboardCollection& collection) {
    ValidateDashboardCollection(collection);
    std::ostringstream output;
    output << "{\n"
           << "  \"version\": 2,\n"
           << "  \"revision\": " << collection.revision << ",\n"
           << "  \"defaultPanel\": \"" << EscapeJson(collection.defaultPanel) << "\",\n"
           << "  \"panels\": [\n";
    for (std::size_t index = 0; index < collection.panels.size(); ++index) {
        SerializePanel(output, collection.panels[index], "    ");
        if (index + 1U != collection.panels.size()) {
            output << ',';
        }
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

DashboardPanel* FindDashboardPanel(DashboardCollection& collection, std::string_view id) {
    const auto iterator = std::find_if(
        collection.panels.begin(), collection.panels.end(),
        [id](const DashboardPanel& panel) { return panel.id == id; });
    return iterator == collection.panels.end() ? nullptr : &*iterator;
}

const DashboardPanel* FindDashboardPanel(
    const DashboardCollection& collection,
    std::string_view id) {
    const auto iterator = std::find_if(
        collection.panels.begin(), collection.panels.end(),
        [id](const DashboardPanel& panel) { return panel.id == id; });
    return iterator == collection.panels.end() ? nullptr : &*iterator;
}

std::vector<DashboardReferenceIssue> InspectDashboardReferences(
    const DashboardCollection& collection,
    const BusesConfig& buses) {
    std::vector<DashboardReferenceIssue> result;
    for (const DashboardPanel& panel : collection.panels) {
        DashboardConfig compatibility;
        compatibility.title = panel.title;
        compatibility.background = panel.background;
        compatibility.fans = panel.fans;
        std::vector<DashboardReferenceIssue> issues =
            InspectDashboardReferences(compatibility, buses);
        for (DashboardReferenceIssue& issue : issues) {
            issue.panelId = panel.id;
            result.push_back(std::move(issue));
        }
    }
    return result;
}

}  // namespace mdvwb
