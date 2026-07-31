#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mdvwb::json {

class ParseError final : public std::runtime_error {
public:
    ParseError(std::size_t line, std::size_t column, std::string message);

    [[nodiscard]] std::size_t Line() const noexcept;
    [[nodiscard]] std::size_t Column() const noexcept;

private:
    std::size_t line_ = 1;
    std::size_t column_ = 1;
};

struct Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value, std::less<>>;

struct Value {
    using Storage = std::variant<
        std::nullptr_t,
        bool,
        std::int64_t,
        double,
        std::string,
        Array,
        Object>;

    Storage data = nullptr;

    Value() = default;
    Value(std::nullptr_t) noexcept;
    Value(bool value) noexcept;
    Value(std::int64_t value) noexcept;
    Value(double value) noexcept;
    Value(std::string value);
    Value(Array value);
    Value(Object value);

    [[nodiscard]] bool IsNull() const noexcept;
    [[nodiscard]] bool IsBoolean() const noexcept;
    [[nodiscard]] bool IsInteger() const noexcept;
    [[nodiscard]] bool IsNumber() const noexcept;
    [[nodiscard]] bool IsString() const noexcept;
    [[nodiscard]] bool IsArray() const noexcept;
    [[nodiscard]] bool IsObject() const noexcept;

    [[nodiscard]] bool AsBoolean() const;
    [[nodiscard]] std::int64_t AsInteger() const;
    [[nodiscard]] double AsNumber() const;
    [[nodiscard]] const std::string& AsString() const;
    [[nodiscard]] const Array& AsArray() const;
    [[nodiscard]] const Object& AsObject() const;
};

[[nodiscard]] Value Parse(std::string_view text);
[[nodiscard]] Value ParseFile(const std::filesystem::path& path);

} // namespace mdvwb::json
