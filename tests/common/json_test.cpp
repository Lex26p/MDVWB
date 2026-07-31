#include "json.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void RequireParseError(Function&& function, std::string_view message)
{
    try {
        function();
    }
    catch (const mdvwb::json::ParseError&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

void TestCompleteDocument()
{
    const auto value = mdvwb::json::Parse(R"json(
{
  "name": "profile",
  "enabled": true,
  "count": 63,
  "scale": 0.1,
  "scientific": 2.5e1,
  "nothing": null,
  "values": [1, 2, 3]
}
)json");

    const auto& object = value.AsObject();
    Require(object.at("name").AsString() == "profile", "string value mismatch");
    Require(object.at("enabled").AsBoolean(), "boolean value mismatch");
    Require(object.at("count").AsInteger() == 63, "integer value mismatch");
    Require(std::abs(object.at("scale").AsNumber() - 0.1) < 1e-12,
            "fraction value mismatch");
    Require(std::abs(object.at("scientific").AsNumber() - 25.0) < 1e-12,
            "exponent value mismatch");
    Require(object.at("nothing").IsNull(), "null value mismatch");

    const auto& values = object.at("values").AsArray();
    Require(values.size() == 3U, "array size mismatch");
    Require(values[2].AsInteger() == 3, "array value mismatch");
}

void TestIntegerAndNumberTypesRemainDistinct()
{
    const auto object = mdvwb::json::Parse(
        R"json({"integer":1,"decimal":1.0,"exponent":1e0})json").AsObject();

    Require(object.at("integer").IsInteger(),
            "plain integer did not remain an integer");
    Require(!object.at("decimal").IsInteger() && object.at("decimal").IsNumber(),
            "decimal was not represented as a floating-point number");
    Require(!object.at("exponent").IsInteger() && object.at("exponent").IsNumber(),
            "exponent was not represented as a floating-point number");
}

void TestUnicodeEscapes()
{
    const auto value = mdvwb::json::Parse(
        R"json({"latin":"\u041C\u0414\u0412","emoji":"\uD83D\uDE80"})json");

    const auto& object = value.AsObject();
    Require(object.at("latin").AsString() == "\xD0\x9C\xD0\x94\xD0\x92",
            "BMP unicode escape was decoded incorrectly");
    Require(object.at("emoji").AsString() == "\xF0\x9F\x9A\x80",
            "unicode surrogate pair was decoded incorrectly");
}

void TestStrictSyntax()
{
    RequireParseError(
        [] { static_cast<void>(mdvwb::json::Parse(R"json({"a":1,"a":2})json")); },
        "duplicate object key was accepted");

    RequireParseError(
        [] { static_cast<void>(mdvwb::json::Parse(R"json({"n":01})json")); },
        "leading zero was accepted");

    RequireParseError(
        [] { static_cast<void>(mdvwb::json::Parse(R"json({"n":1.})json")); },
        "unfinished fraction was accepted");

    RequireParseError(
        [] { static_cast<void>(mdvwb::json::Parse(R"json({"n":1e})json")); },
        "unfinished exponent was accepted");

    RequireParseError(
        [] { static_cast<void>(mdvwb::json::Parse(R"json({"x":"\uD800"})json")); },
        "unpaired high surrogate was accepted");

    RequireParseError(
        [] { static_cast<void>(mdvwb::json::Parse("true false")); },
        "trailing root data was accepted");
}

void TestParseErrorLocation()
{
    try {
        static_cast<void>(mdvwb::json::Parse("{\n  \"a\": 1,\n  ]"));
    }
    catch (const mdvwb::json::ParseError& error) {
        Require(error.Line() == 3U, "parse error line is incorrect");
        Require(error.Column() >= 3U, "parse error column is incorrect");
        return;
    }

    throw std::runtime_error("invalid JSON did not report a parse error");
}

void TestFileLoading()
{
    const auto path =
        std::filesystem::temp_directory_path() /
        "mdvwb-json-parser-self-test.json";

    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("could not create temporary JSON file");
        }
        output << R"json({"schemaVersion":1,"scale":0.5})json";
    }

    try {
        const auto value = mdvwb::json::ParseFile(path);
        const auto& object = value.AsObject();
        Require(object.at("schemaVersion").AsInteger() == 1,
                "file integer value mismatch");
        Require(std::abs(object.at("scale").AsNumber() - 0.5) < 1e-12,
                "file decimal value mismatch");
    }
    catch (...) {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        throw;
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

} // namespace

int main()
{
    try {
        TestCompleteDocument();
        TestIntegerAndNumberTypesRemainDistinct();
        TestUnicodeEscapes();
        TestStrictSyntax();
        TestParseErrorLocation();
        TestFileLoading();

        std::cout << "MDVWB JSON tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB JSON tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
