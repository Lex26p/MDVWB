#include "mdv_schedules_config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
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
            case 't': ConsumeLiteral("true"); return JsonValue{true};
            case 'f': ConsumeLiteral("false"); return JsonValue{false};
            case 'n': ConsumeLiteral("null"); return JsonValue{nullptr};
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
            switch (Consume()) {
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
        throw SchedulesConfigError(
            "JSON error at line " + std::to_string(line) +
            ", column " + std::to_string(column) + ": " + message);
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

const JsonObject& RequireObject(const JsonValue& value, std::string_view path) {
    const auto* object = std::get_if<JsonObject>(&value.value);
    if (object == nullptr) {
        throw SchedulesConfigError(std::string(path) + " must be an object");
    }
    return *object;
}

const JsonArray& RequireArray(const JsonValue& value, std::string_view path) {
    const auto* array = std::get_if<JsonArray>(&value.value);
    if (array == nullptr) {
        throw SchedulesConfigError(std::string(path) + " must be an array");
    }
    return *array;
}

const std::string& RequireString(const JsonValue& value, std::string_view path) {
    const auto* string = std::get_if<std::string>(&value.value);
    if (string == nullptr) {
        throw SchedulesConfigError(std::string(path) + " must be a string");
    }
    return *string;
}

bool RequireBoolean(const JsonValue& value, std::string_view path) {
    const auto* boolean = std::get_if<bool>(&value.value);
    if (boolean == nullptr) {
        throw SchedulesConfigError(std::string(path) + " must be true or false");
    }
    return *boolean;
}

double RequireNumber(const JsonValue& value, std::string_view path) {
    const auto* number = std::get_if<double>(&value.value);
    if (number == nullptr || !std::isfinite(*number)) {
        throw SchedulesConfigError(std::string(path) + " must be a finite number");
    }
    return *number;
}

const JsonValue& RequireField(
    const JsonObject& object,
    std::string_view key,
    std::string_view path) {
    const auto iterator = object.find(std::string(key));
    if (iterator == object.end()) {
        throw SchedulesConfigError(
            std::string(path) + " is missing required field '" + std::string(key) + "'");
    }
    return iterator->second;
}

const JsonValue* OptionalField(const JsonObject& object, std::string_view key) {
    const auto iterator = object.find(std::string(key));
    return iterator == object.end() ? nullptr : &iterator->second;
}

void RejectUnknownFields(
    const JsonObject& object,
    const std::set<std::string>& allowed,
    std::string_view path) {
    for (const auto& [key, unused] : object) {
        static_cast<void>(unused);
        if (allowed.find(key) == allowed.end()) {
            throw SchedulesConfigError(std::string(path) + " contains unknown field '" + key + "'");
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
        throw SchedulesConfigError(std::string(path) + " must be an integer");
    }
    if (number < static_cast<double>(minimum) || number > static_cast<double>(maximum)) {
        throw SchedulesConfigError(
            std::string(path) + " must be in range " + std::to_string(minimum) +
            ".." + std::to_string(maximum));
    }
    return static_cast<int>(number);
}

void RequireLength(
    const std::string& value,
    std::size_t minimum,
    std::size_t maximum,
    std::string_view path) {
    if (value.size() < minimum || value.size() > maximum) {
        throw SchedulesConfigError(
            std::string(path) + " length must be in range " + std::to_string(minimum) +
            ".." + std::to_string(maximum) + " bytes");
    }
}

bool IsSafeIdentifier(std::string_view value) {
    if (value.empty() || value.size() > 64U) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](char ch) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        return std::isalnum(byte) != 0 || ch == '_' || ch == '-';
    });
}

bool IsValidTime(std::string_view value) {
    if (value.size() != 5U || value[2] != ':' ||
        !std::isdigit(static_cast<unsigned char>(value[0])) ||
        !std::isdigit(static_cast<unsigned char>(value[1])) ||
        !std::isdigit(static_cast<unsigned char>(value[3])) ||
        !std::isdigit(static_cast<unsigned char>(value[4]))) {
        return false;
    }
    const int hour = (value[0] - '0') * 10 + (value[1] - '0');
    const int minute = (value[3] - '0') * 10 + (value[4] - '0');
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

bool IsValidDate(std::string_view value) {
    if (value.size() != 10U || value[4] != '-' || value[7] != '-') {
        return false;
    }
    for (std::size_t index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U}) {
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
        (value[2] - '0') * 10 + (value[3] - '0');
    const int month = (value[5] - '0') * 10 + (value[6] - '0');
    const int day = (value[8] - '0') * 10 + (value[9] - '0');
    if (year < 2000 || year > 2099 || month < 1 || month > 12) {
        return false;
    }
    static constexpr int DaysPerMonth[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maximum = DaysPerMonth[month - 1];
    if (month == 2 && IsLeapYear(year)) {
        maximum = 29;
    }
    return day >= 1 && day <= maximum;
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

ScheduleActions ParseActions(const JsonValue& value, std::string_view path) {
    const JsonObject& object = RequireObject(value, path);
    RejectUnknownFields(object, {"power", "mode", "speed", "setTemp"}, path);
    ScheduleActions actions;
    if (const JsonValue* field = OptionalField(object, "power")) {
        actions.power = RequireBoolean(*field, std::string(path) + ".power");
    }
    if (const JsonValue* field = OptionalField(object, "mode")) {
        actions.mode = RequireIntegerInRange(*field, 0, 4, std::string(path) + ".mode");
    }
    if (const JsonValue* field = OptionalField(object, "speed")) {
        actions.speed = RequireIntegerInRange(*field, 1, 4, std::string(path) + ".speed");
    }
    if (const JsonValue* field = OptionalField(object, "setTemp")) {
        actions.setTemp = RequireIntegerInRange(*field, 16, 32, std::string(path) + ".setTemp");
    }
    if (!actions.power && !actions.mode && !actions.speed && !actions.setTemp) {
        throw SchedulesConfigError(std::string(path) + " must contain at least one action");
    }
    return actions;
}

ScheduleEntry ParseSchedule(const JsonValue& value, std::size_t index) {
    const std::string path = "root.schedules[" + std::to_string(index) + "]";
    const JsonObject& object = RequireObject(value, path);
    RejectUnknownFields(
        object,
        {"id", "name", "enabled", "panelId", "kind", "days", "date", "time", "targets", "actions"},
        path);

    ScheduleEntry schedule;
    schedule.id = RequireString(RequireField(object, "id", path), path + ".id");
    schedule.name = RequireString(RequireField(object, "name", path), path + ".name");
    schedule.enabled = RequireBoolean(RequireField(object, "enabled", path), path + ".enabled");
    schedule.panelId = RequireString(RequireField(object, "panelId", path), path + ".panelId");
    const std::string& kind = RequireString(RequireField(object, "kind", path), path + ".kind");
    schedule.time = RequireString(RequireField(object, "time", path), path + ".time");
    schedule.date = RequireString(RequireField(object, "date", path), path + ".date");

    if (!IsSafeIdentifier(schedule.id)) {
        throw SchedulesConfigError(path + ".id must contain only letters, digits, '-' or '_'");
    }
    RequireLength(schedule.name, 1U, 128U, path + ".name");
    if (!IsSafeIdentifier(schedule.panelId)) {
        throw SchedulesConfigError(path + ".panelId must be a safe panel identifier");
    }
    if (kind == "weekly") {
        schedule.kind = ScheduleKind::Weekly;
    } else if (kind == "once") {
        schedule.kind = ScheduleKind::Once;
    } else {
        throw SchedulesConfigError(path + ".kind must be 'weekly' or 'once'");
    }
    if (!IsValidTime(schedule.time)) {
        throw SchedulesConfigError(path + ".time must use HH:MM in range 00:00..23:59");
    }

    const JsonArray& days = RequireArray(RequireField(object, "days", path), path + ".days");
    std::set<int> uniqueDays;
    for (std::size_t dayIndex = 0; dayIndex < days.size(); ++dayIndex) {
        const int day = RequireIntegerInRange(
            days[dayIndex], 1, 7,
            path + ".days[" + std::to_string(dayIndex) + "]");
        if (!uniqueDays.insert(day).second) {
            throw SchedulesConfigError(path + ".days contains duplicate day " + std::to_string(day));
        }
        schedule.days.push_back(day);
    }
    std::sort(schedule.days.begin(), schedule.days.end());

    if (schedule.kind == ScheduleKind::Weekly) {
        if (schedule.days.empty()) {
            throw SchedulesConfigError(path + ".days must not be empty for weekly schedule");
        }
        if (!schedule.date.empty()) {
            throw SchedulesConfigError(path + ".date must be empty for weekly schedule");
        }
    } else {
        if (!schedule.days.empty()) {
            throw SchedulesConfigError(path + ".days must be empty for once schedule");
        }
        if (!IsValidDate(schedule.date)) {
            throw SchedulesConfigError(path + ".date must use a valid YYYY-MM-DD date in 2000..2099");
        }
    }

    const JsonArray& targets = RequireArray(
        RequireField(object, "targets", path), path + ".targets");
    if (targets.empty() || targets.size() > 512U) {
        throw SchedulesConfigError(path + ".targets count must be in range 1..512");
    }
    std::set<std::pair<int, int>> uniqueTargets;
    for (std::size_t targetIndex = 0; targetIndex < targets.size(); ++targetIndex) {
        const std::string targetPath =
            path + ".targets[" + std::to_string(targetIndex) + "]";
        const JsonObject& targetObject = RequireObject(targets[targetIndex], targetPath);
        RejectUnknownFields(targetObject, {"bus", "address"}, targetPath);
        ScheduleTarget target;
        target.bus = RequireIntegerInRange(
            RequireField(targetObject, "bus", targetPath), 1, 999, targetPath + ".bus");
        target.address = RequireIntegerInRange(
            RequireField(targetObject, "address", targetPath), 0, 63, targetPath + ".address");
        if (!uniqueTargets.emplace(target.bus, target.address).second) {
            throw SchedulesConfigError(
                targetPath + " duplicates target " + std::to_string(target.bus) + "/" +
                std::to_string(target.address));
        }
        schedule.targets.push_back(target);
    }
    std::sort(schedule.targets.begin(), schedule.targets.end(), [](const auto& left, const auto& right) {
        return std::tie(left.bus, left.address) < std::tie(right.bus, right.address);
    });

    schedule.actions = ParseActions(RequireField(object, "actions", path), path + ".actions");
    return schedule;
}

SchedulesConfig ValidateAndConvert(const JsonValue& rootValue) {
    const JsonObject& root = RequireObject(rootValue, "root");
    RejectUnknownFields(root, {"version", "revision", "schedules"}, "root");

    SchedulesConfig result;
    result.version = RequireIntegerInRange(
        RequireField(root, "version", "root"), 1, 1, "root.version");
    result.revision = RequireIntegerInRange(
        RequireField(root, "revision", "root"), 0, 2147483647, "root.revision");

    const JsonArray& schedules = RequireArray(
        RequireField(root, "schedules", "root"), "root.schedules");
    if (schedules.size() > 256U) {
        throw SchedulesConfigError("root.schedules must contain at most 256 entries");
    }
    std::set<std::string> uniqueIds;
    for (std::size_t index = 0; index < schedules.size(); ++index) {
        ScheduleEntry schedule = ParseSchedule(schedules[index], index);
        if (!uniqueIds.insert(schedule.id).second) {
            throw SchedulesConfigError("duplicate schedule id '" + schedule.id + "'");
        }
        result.schedules.push_back(std::move(schedule));
    }
    std::sort(result.schedules.begin(), result.schedules.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return result;
}

std::string KindText(ScheduleKind kind) {
    return kind == ScheduleKind::Weekly ? "weekly" : "once";
}

std::string ReferenceIssueText(const ScheduleReferenceIssue& issue) {
    const std::string prefix = "schedule '" + issue.scheduleId + "'";
    switch (issue.kind) {
        case ScheduleReferenceIssueKind::MissingPanel:
            return prefix + " references missing panel '" + issue.panelId + "'";
        case ScheduleReferenceIssueKind::MissingBus:
            return prefix + " references missing bus " + std::to_string(issue.bus);
        case ScheduleReferenceIssueKind::MissingAddress:
            return prefix + " references missing address " + std::to_string(issue.bus) + "/" +
                std::to_string(issue.address);
        case ScheduleReferenceIssueKind::TargetNotInPanel:
            return prefix + " target " + std::to_string(issue.bus) + "/" +
                std::to_string(issue.address) + " is not visible in panel '" + issue.panelId + "'";
    }
    return prefix + " has an unknown reference issue";
}

}  // namespace

SchedulesConfig ParseSchedulesConfig(std::string_view jsonText) {
    return ValidateAndConvert(JsonParser(jsonText).Parse());
}

SchedulesConfig LoadSchedulesConfig(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw SchedulesConfigError("cannot open schedules configuration '" + path.string() + "'");
    }
    const std::string text{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (!input.eof() && input.fail()) {
        throw SchedulesConfigError("cannot read schedules configuration '" + path.string() + "'");
    }
    return ParseSchedulesConfig(text);
}

std::string SerializeSchedulesConfig(const SchedulesConfig& config) {
    SchedulesConfig normalized = config;
    normalized = ParseSchedulesConfig([&]() {
        std::ostringstream output;
        output << "{\"version\":" << config.version
               << ",\"revision\":" << config.revision << ",\"schedules\":[";
        for (std::size_t index = 0; index < config.schedules.size(); ++index) {
            const ScheduleEntry& schedule = config.schedules[index];
            if (index != 0U) output << ',';
            output << "{\"id\":\"" << EscapeJson(schedule.id)
                   << "\",\"name\":\"" << EscapeJson(schedule.name)
                   << "\",\"enabled\":" << (schedule.enabled ? "true" : "false")
                   << ",\"panelId\":\"" << EscapeJson(schedule.panelId)
                   << "\",\"kind\":\"" << KindText(schedule.kind)
                   << "\",\"days\":[";
            for (std::size_t dayIndex = 0; dayIndex < schedule.days.size(); ++dayIndex) {
                if (dayIndex != 0U) output << ',';
                output << schedule.days[dayIndex];
            }
            output << "],\"date\":\"" << EscapeJson(schedule.date)
                   << "\",\"time\":\"" << EscapeJson(schedule.time)
                   << "\",\"targets\":[";
            for (std::size_t targetIndex = 0; targetIndex < schedule.targets.size(); ++targetIndex) {
                if (targetIndex != 0U) output << ',';
                output << "{\"bus\":" << schedule.targets[targetIndex].bus
                       << ",\"address\":" << schedule.targets[targetIndex].address << '}';
            }
            output << "],\"actions\":{";
            bool needsComma = false;
            auto append = [&](std::string_view key, std::string value) {
                if (needsComma) output << ',';
                output << '\"' << key << "\":" << value;
                needsComma = true;
            };
            if (schedule.actions.power) append("power", *schedule.actions.power ? "true" : "false");
            if (schedule.actions.mode) append("mode", std::to_string(*schedule.actions.mode));
            if (schedule.actions.speed) append("speed", std::to_string(*schedule.actions.speed));
            if (schedule.actions.setTemp) append("setTemp", std::to_string(*schedule.actions.setTemp));
            output << "}}";
        }
        output << "]}";
        return output.str();
    }());

    std::ostringstream output;
    output << "{\n  \"version\": 1,\n  \"revision\": " << normalized.revision
           << ",\n  \"schedules\": [";
    for (std::size_t index = 0; index < normalized.schedules.size(); ++index) {
        const ScheduleEntry& schedule = normalized.schedules[index];
        output << (index == 0U ? "\n" : ",\n")
               << "    {\n"
               << "      \"id\": \"" << EscapeJson(schedule.id) << "\",\n"
               << "      \"name\": \"" << EscapeJson(schedule.name) << "\",\n"
               << "      \"enabled\": " << (schedule.enabled ? "true" : "false") << ",\n"
               << "      \"panelId\": \"" << EscapeJson(schedule.panelId) << "\",\n"
               << "      \"kind\": \"" << KindText(schedule.kind) << "\",\n"
               << "      \"days\": [";
        for (std::size_t dayIndex = 0; dayIndex < schedule.days.size(); ++dayIndex) {
            if (dayIndex != 0U) output << ", ";
            output << schedule.days[dayIndex];
        }
        output << "],\n"
               << "      \"date\": \"" << EscapeJson(schedule.date) << "\",\n"
               << "      \"time\": \"" << EscapeJson(schedule.time) << "\",\n"
               << "      \"targets\": [";
        for (std::size_t targetIndex = 0; targetIndex < schedule.targets.size(); ++targetIndex) {
            if (targetIndex != 0U) output << ", ";
            output << "{\"bus\": " << schedule.targets[targetIndex].bus
                   << ", \"address\": " << schedule.targets[targetIndex].address << '}';
        }
        output << "],\n      \"actions\": {";
        bool needsComma = false;
        auto append = [&](std::string_view key, std::string value) {
            if (needsComma) output << ", ";
            output << '\"' << key << "\": " << value;
            needsComma = true;
        };
        if (schedule.actions.power) append("power", *schedule.actions.power ? "true" : "false");
        if (schedule.actions.mode) append("mode", std::to_string(*schedule.actions.mode));
        if (schedule.actions.speed) append("speed", std::to_string(*schedule.actions.speed));
        if (schedule.actions.setTemp) append("setTemp", std::to_string(*schedule.actions.setTemp));
        output << "}\n    }";
    }
    if (!normalized.schedules.empty()) {
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

const ScheduleEntry* FindSchedule(const SchedulesConfig& config, std::string_view id) {
    const auto iterator = std::find_if(config.schedules.begin(), config.schedules.end(),
        [&](const ScheduleEntry& schedule) { return schedule.id == id; });
    return iterator == config.schedules.end() ? nullptr : &*iterator;
}

ScheduleEntry* FindSchedule(SchedulesConfig& config, std::string_view id) {
    return const_cast<ScheduleEntry*>(FindSchedule(std::as_const(config), id));
}

std::vector<ScheduleReferenceIssue> InspectScheduleReferences(
    const SchedulesConfig& schedules,
    const BusesConfig& buses,
    const DashboardCollection& dashboards) {
    std::vector<ScheduleReferenceIssue> issues;
    for (const ScheduleEntry& schedule : schedules.schedules) {
        const DashboardPanel* panel = FindDashboardPanel(dashboards, schedule.panelId);
        if (panel == nullptr) {
            issues.push_back({
                ScheduleReferenceIssueKind::MissingPanel,
                schedule.id,
                schedule.panelId,
                0,
                0});
        }
        for (const ScheduleTarget& target : schedule.targets) {
            const auto busIterator = std::find_if(buses.buses.begin(), buses.buses.end(),
                [&](const BusConfig& bus) { return bus.id == target.bus; });
            if (busIterator == buses.buses.end()) {
                issues.push_back({
                    ScheduleReferenceIssueKind::MissingBus,
                    schedule.id,
                    schedule.panelId,
                    target.bus,
                    target.address});
                continue;
            }
            if (std::find(busIterator->addresses.begin(), busIterator->addresses.end(), target.address) ==
                busIterator->addresses.end()) {
                issues.push_back({
                    ScheduleReferenceIssueKind::MissingAddress,
                    schedule.id,
                    schedule.panelId,
                    target.bus,
                    target.address});
                continue;
            }
            if (panel != nullptr) {
                const bool isVisible = std::any_of(panel->fans.begin(), panel->fans.end(),
                    [&](const DashboardFanPlacement& fan) {
                        return fan.visible && fan.bus == target.bus && fan.address == target.address;
                    });
                if (!isVisible) {
                    issues.push_back({
                        ScheduleReferenceIssueKind::TargetNotInPanel,
                        schedule.id,
                        schedule.panelId,
                        target.bus,
                        target.address});
                }
            }
        }
    }
    return issues;
}

void ValidateScheduleReferences(
    const SchedulesConfig& schedules,
    const BusesConfig& buses,
    const DashboardCollection& dashboards) {
    const auto issues = InspectScheduleReferences(schedules, buses, dashboards);
    if (!issues.empty()) {
        throw SchedulesConfigError(
            ReferenceIssueText(issues.front()) +
            (issues.size() == 1U ? std::string{} :
                " (and " + std::to_string(issues.size() - 1U) + " more reference issues)"));
    }
}

}  // namespace mdvwb
