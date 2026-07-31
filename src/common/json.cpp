#include "json.h"

#include <charconv>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace mdvwb::json {
namespace {

[[nodiscard]] std::string TypeError(std::string_view expected)
{
    return "JSON value is not " + std::string(expected);
}

class Parser final {
public:
    explicit Parser(std::string_view text)
        : text_(text)
    {
    }

    [[nodiscard]] Value Run()
    {
        SkipWhitespace();
        auto value = ParseValue();
        SkipWhitespace();
        if (!AtEnd()) {
            Fail("unexpected characters after the root value");
        }
        return value;
    }

private:
    [[nodiscard]] Value ParseValue()
    {
        if (AtEnd()) {
            Fail("unexpected end of JSON");
        }

        switch (Peek()) {
        case '{':
            return Value(ParseObject());
        case '[':
            return Value(ParseArray());
        case '"':
            return Value(ParseString());
        case 't':
            ConsumeLiteral("true");
            return Value(true);
        case 'f':
            ConsumeLiteral("false");
            return Value(false);
        case 'n':
            ConsumeLiteral("null");
            return Value(nullptr);
        default:
            if (Peek() == '-' || IsDigit(Peek())) {
                return ParseNumber();
            }
            Fail("expected an object, array, string, number, boolean or null");
        }
    }

    [[nodiscard]] Object ParseObject()
    {
        Expect('{');
        SkipWhitespace();

        Object result;
        if (TryConsume('}')) {
            return result;
        }

        while (true) {
            if (AtEnd() || Peek() != '"') {
                Fail("expected an object key");
            }

            auto key = ParseString();
            SkipWhitespace();
            Expect(':');
            SkipWhitespace();

            if (result.contains(key)) {
                Fail("duplicate object key '" + key + "'");
            }

            result.emplace(std::move(key), ParseValue());
            SkipWhitespace();

            if (TryConsume('}')) {
                return result;
            }

            Expect(',');
            SkipWhitespace();
        }
    }

    [[nodiscard]] Array ParseArray()
    {
        Expect('[');
        SkipWhitespace();

        Array result;
        if (TryConsume(']')) {
            return result;
        }

        while (true) {
            result.push_back(ParseValue());
            SkipWhitespace();

            if (TryConsume(']')) {
                return result;
            }

            Expect(',');
            SkipWhitespace();
        }
    }

    [[nodiscard]] std::string ParseString()
    {
        Expect('"');

        std::string result;
        while (!AtEnd()) {
            const auto character = Consume();
            if (character == '"') {
                return result;
            }

            if (static_cast<unsigned char>(character) < 0x20U) {
                Fail("control character inside a string");
            }

            if (character != '\\') {
                result.push_back(character);
                continue;
            }

            if (AtEnd()) {
                Fail("unfinished string escape");
            }

            switch (Consume()) {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u':
                AppendEscapedCodePoint(result);
                break;
            default:
                Fail("invalid string escape");
            }
        }

        Fail("unterminated string");
    }

    [[nodiscard]] Value ParseNumber()
    {
        const auto begin = position_;
        bool floating = false;

        TryConsume('-');

        if (AtEnd()) {
            Fail("unfinished number");
        }

        if (Peek() == '0') {
            ++position_;
            if (!AtEnd() && IsDigit(Peek())) {
                Fail("leading zero in number");
            }
        }
        else {
            if (!IsDigitOneToNine(Peek())) {
                Fail("invalid number");
            }
            while (!AtEnd() && IsDigit(Peek())) {
                ++position_;
            }
        }

        if (!AtEnd() && Peek() == '.') {
            floating = true;
            ++position_;

            if (AtEnd() || !IsDigit(Peek())) {
                Fail("fraction must contain at least one digit");
            }
            while (!AtEnd() && IsDigit(Peek())) {
                ++position_;
            }
        }

        if (!AtEnd() && (Peek() == 'e' || Peek() == 'E')) {
            floating = true;
            ++position_;

            if (!AtEnd() && (Peek() == '+' || Peek() == '-')) {
                ++position_;
            }

            if (AtEnd() || !IsDigit(Peek())) {
                Fail("exponent must contain at least one digit");
            }
            while (!AtEnd() && IsDigit(Peek())) {
                ++position_;
            }
        }

        const auto token = text_.substr(begin, position_ - begin);

        if (!floating) {
            std::int64_t value = 0;
            const auto result = std::from_chars(
                token.data(), token.data() + token.size(), value, 10);
            if (result.ec == std::errc{} &&
                result.ptr == token.data() + token.size()) {
                return Value(value);
            }
            if (result.ec == std::errc::result_out_of_range) {
                Fail("integer is outside signed 64-bit range");
            }
            Fail("invalid integer");
        }

        double value = 0.0;
        const auto result = std::from_chars(
            token.data(),
            token.data() + token.size(),
            value,
            std::chars_format::general);
        if (result.ec == std::errc::result_out_of_range || !std::isfinite(value)) {
            Fail("floating-point number is outside the supported range");
        }
        if (result.ec != std::errc{} ||
            result.ptr != token.data() + token.size()) {
            Fail("invalid floating-point number");
        }
        return Value(value);
    }

    void AppendEscapedCodePoint(std::string& output)
    {
        auto codePoint = ParseHex4();

        if (codePoint >= 0xD800U && codePoint <= 0xDBFFU) {
            if (AtEnd() || Consume() != '\\' || AtEnd() || Consume() != 'u') {
                Fail("high unicode surrogate must be followed by a low surrogate");
            }

            const auto low = ParseHex4();
            if (low < 0xDC00U || low > 0xDFFFU) {
                Fail("invalid low unicode surrogate");
            }

            codePoint =
                0x10000U +
                ((codePoint - 0xD800U) << 10U) +
                (low - 0xDC00U);
        }
        else if (codePoint >= 0xDC00U && codePoint <= 0xDFFFU) {
            Fail("unexpected low unicode surrogate");
        }

        AppendUtf8(output, codePoint);
    }

    [[nodiscard]] unsigned int ParseHex4()
    {
        unsigned int result = 0;
        for (int index = 0; index < 4; ++index) {
            if (AtEnd()) {
                Fail("unfinished unicode escape");
            }

            const auto character = Consume();
            result <<= 4U;

            if (character >= '0' && character <= '9') {
                result += static_cast<unsigned int>(character - '0');
            }
            else if (character >= 'a' && character <= 'f') {
                result += static_cast<unsigned int>(character - 'a' + 10);
            }
            else if (character >= 'A' && character <= 'F') {
                result += static_cast<unsigned int>(character - 'A' + 10);
            }
            else {
                Fail("invalid unicode escape");
            }
        }
        return result;
    }

    static void AppendUtf8(std::string& output, unsigned int codePoint)
    {
        if (codePoint <= 0x7FU) {
            output.push_back(static_cast<char>(codePoint));
            return;
        }

        if (codePoint <= 0x7FFU) {
            output.push_back(static_cast<char>(0xC0U | (codePoint >> 6U)));
            output.push_back(
                static_cast<char>(0x80U | (codePoint & 0x3FU)));
            return;
        }

        if (codePoint <= 0xFFFFU) {
            output.push_back(static_cast<char>(0xE0U | (codePoint >> 12U)));
            output.push_back(
                static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
            output.push_back(
                static_cast<char>(0x80U | (codePoint & 0x3FU)));
            return;
        }

        output.push_back(static_cast<char>(0xF0U | (codePoint >> 18U)));
        output.push_back(
            static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU)));
        output.push_back(
            static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU)));
        output.push_back(
            static_cast<char>(0x80U | (codePoint & 0x3FU)));
    }

    void ConsumeLiteral(std::string_view literal)
    {
        for (const auto expected : literal) {
            if (AtEnd() || Consume() != expected) {
                Fail("invalid literal");
            }
        }
    }

    void SkipWhitespace() noexcept
    {
        while (!AtEnd()) {
            const auto character = Peek();
            if (character != ' ' &&
                character != '\t' &&
                character != '\r' &&
                character != '\n') {
                return;
            }
            ++position_;
        }
    }

    void Expect(char expected)
    {
        if (AtEnd() || Consume() != expected) {
            Fail(std::string("expected '") + expected + "'");
        }
    }

    [[nodiscard]] bool TryConsume(char expected) noexcept
    {
        if (!AtEnd() && Peek() == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool AtEnd() const noexcept
    {
        return position_ >= text_.size();
    }

    [[nodiscard]] char Peek() const noexcept
    {
        return text_[position_];
    }

    [[nodiscard]] char Consume() noexcept
    {
        return text_[position_++];
    }

    [[nodiscard]] static bool IsDigit(char value) noexcept
    {
        return value >= '0' && value <= '9';
    }

    [[nodiscard]] static bool IsDigitOneToNine(char value) noexcept
    {
        return value >= '1' && value <= '9';
    }

    [[noreturn]] void Fail(std::string message) const
    {
        std::size_t line = 1;
        std::size_t column = 1;

        for (std::size_t index = 0;
             index < position_ && index < text_.size();
             ++index) {
            if (text_[index] == '\n') {
                ++line;
                column = 1;
            }
            else {
                ++column;
            }
        }

        throw ParseError(line, column, std::move(message));
    }

    std::string_view text_;
    std::size_t position_ = 0;
};

template <typename T>
[[nodiscard]] const T& Require(
    const Value::Storage& storage,
    std::string_view expected)
{
    const auto* value = std::get_if<T>(&storage);
    if (value == nullptr) {
        throw std::logic_error(TypeError(expected));
    }
    return *value;
}

} // namespace

ParseError::ParseError(
    std::size_t line,
    std::size_t column,
    std::string message)
    : std::runtime_error(
          "JSON error at line " + std::to_string(line) +
          ", column " + std::to_string(column) + ": " + message),
      line_(line),
      column_(column)
{
}

std::size_t ParseError::Line() const noexcept
{
    return line_;
}

std::size_t ParseError::Column() const noexcept
{
    return column_;
}

Value::Value(std::nullptr_t) noexcept
    : data(nullptr)
{
}

Value::Value(bool value) noexcept
    : data(value)
{
}

Value::Value(std::int64_t value) noexcept
    : data(value)
{
}

Value::Value(double value) noexcept
    : data(value)
{
}

Value::Value(std::string value)
    : data(std::move(value))
{
}

Value::Value(Array value)
    : data(std::move(value))
{
}

Value::Value(Object value)
    : data(std::move(value))
{
}

bool Value::IsNull() const noexcept
{
    return std::holds_alternative<std::nullptr_t>(data);
}

bool Value::IsBoolean() const noexcept
{
    return std::holds_alternative<bool>(data);
}

bool Value::IsInteger() const noexcept
{
    return std::holds_alternative<std::int64_t>(data);
}

bool Value::IsNumber() const noexcept
{
    return IsInteger() || std::holds_alternative<double>(data);
}

bool Value::IsString() const noexcept
{
    return std::holds_alternative<std::string>(data);
}

bool Value::IsArray() const noexcept
{
    return std::holds_alternative<Array>(data);
}

bool Value::IsObject() const noexcept
{
    return std::holds_alternative<Object>(data);
}

bool Value::AsBoolean() const
{
    return Require<bool>(data, "a boolean");
}

std::int64_t Value::AsInteger() const
{
    return Require<std::int64_t>(data, "an integer");
}

double Value::AsNumber() const
{
    if (const auto* integer = std::get_if<std::int64_t>(&data)) {
        return static_cast<double>(*integer);
    }
    return Require<double>(data, "a number");
}

const std::string& Value::AsString() const
{
    return Require<std::string>(data, "a string");
}

const Array& Value::AsArray() const
{
    return Require<Array>(data, "an array");
}

const Object& Value::AsObject() const
{
    return Require<Object>(data, "an object");
}

Value Parse(std::string_view text)
{
    return Parser(text).Run();
}

Value ParseFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot open JSON file '" + path.string() + "'");
    }

    const std::string text{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};

    if (input.bad()) {
        throw std::runtime_error(
            "cannot read JSON file '" + path.string() + "'");
    }

    return Parse(text);
}

} // namespace mdvwb::json
