#include "modbus_profile.h"
#include "modbus_value.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void RequireConversionError(
    Function&& function,
    std::string_view expectedText)
{
    try {
        function();
    }
    catch (const mdv::modbus::ValueConversionError& error) {
        if (std::string_view(error.what()).find(expectedText) ==
            std::string_view::npos) {
            throw std::runtime_error(
                "conversion failed for the wrong reason: " +
                std::string(error.what()));
        }
        return;
    }

    throw std::runtime_error(
        "invalid conversion was accepted: " + std::string(expectedText));
}

mdv::modbus::PointDefinition NumberPoint(
    double scale = 1.0,
    double offset = 0.0)
{
    mdv::modbus::PointDefinition point;
    point.type = mdv::modbus::PointType::Number;
    point.transform = mdv::modbus::NumericTransform{
        .scale = scale,
        .offset = offset,
    };
    return point;
}

void TestBooleanConversion()
{
    mdv::modbus::PointDefinition point;
    point.type = mdv::modbus::PointType::Boolean;

    Require(
        std::get<bool>(mdv::modbus::DecodePointValue(point, 0U)) == false,
        "boolean raw 0 did not decode to false");
    Require(
        std::get<bool>(mdv::modbus::DecodePointValue(point, 1U)) == true,
        "boolean raw 1 did not decode to true");
    Require(
        mdv::modbus::EncodePointValue(point, mdv::modbus::PointValue(false)) == 0U,
        "boolean false did not encode to raw 0");
    Require(
        mdv::modbus::EncodePointValue(point, mdv::modbus::PointValue(true)) == 1U,
        "boolean true did not encode to raw 1");

    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::DecodePointValue(point, 2U));
        },
        "exactly 0 or 1");
}

void TestEnumConversion()
{
    mdv::modbus::PointDefinition point;
    point.type = mdv::modbus::PointType::Enum;
    point.enumMappings.read = {
        {10U, "cool"},
        {20U, "heat"},
    };
    point.enumMappings.write = {
        {"cool", 1U},
        {"heat", 2U},
    };

    Require(
        std::get<std::string>(
            mdv::modbus::DecodePointValue(point, 10U)) == "cool",
        "enum readMap conversion mismatch");
    Require(
        mdv::modbus::EncodePointValue(
            point,
            mdv::modbus::PointValue(std::string("heat"))) == 2U,
        "enum writeMap conversion mismatch");

    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::DecodePointValue(point, 30U));
        },
        "not present in readMap");
    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodePointValue(
                    point,
                    mdv::modbus::PointValue(std::string("auto"))));
        },
        "not present in writeMap");
}

void TestUnsignedNumericScaling()
{
    const auto point = NumberPoint(0.1, -5.0);

    const auto decoded = std::get<double>(
        mdv::modbus::DecodePointValue(point, 275U));
    Require(std::abs(decoded - 22.5) < 1e-12,
            "uint16 numeric read scaling mismatch");

    Require(
        mdv::modbus::EncodePointValue(
            point,
            mdv::modbus::PointValue(22.5)) == 275U,
        "uint16 inverse scaling mismatch");
}

void TestSignedInt16Conversion()
{
    auto point = NumberPoint(0.1, 0.0);
    point.rawType = mdv::modbus::RawType::Int16;

    const auto decoded = std::get<double>(
        mdv::modbus::DecodePointValue(point, 0xFF85U));
    Require(std::abs(decoded - (-12.3)) < 1e-12,
            "int16 negative raw value decoded incorrectly");

    Require(
        mdv::modbus::EncodePointValue(
            point,
            mdv::modbus::PointValue(-12.3)) == 0xFF85U,
        "negative semantic value encoded incorrectly as int16");
}

void TestLimitsAndStep()
{
    auto point = NumberPoint(0.1, 0.0);
    point.limits = mdv::modbus::NumericLimits{
        .minimum = 16.0,
        .maximum = 30.0,
        .step = 0.5,
    };

    Require(
        mdv::modbus::EncodePointValue(
            point,
            mdv::modbus::PointValue(22.5)) == 225U,
        "valid stepped setpoint was rejected");

    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodePointValue(
                    point,
                    mdv::modbus::PointValue(15.5)));
        },
        "below minimum");

    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodePointValue(
                    point,
                    mdv::modbus::PointValue(30.5)));
        },
        "above maximum");

    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodePointValue(
                    point,
                    mdv::modbus::PointValue(22.25)));
        },
        "configured step");
}

void TestExactAndDeclaredRounding()
{
    auto exact = NumberPoint(0.2, 0.0);
    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodePointValue(
                    exact,
                    mdv::modbus::PointValue(1.1)));
        },
        "represented exactly");

    auto nearest = exact;
    nearest.rounding = mdv::modbus::WriteRounding::Nearest;
    Require(
        mdv::modbus::EncodePointValue(
            nearest,
            mdv::modbus::PointValue(1.1)) == 6U,
        "nearest rounding mismatch");

    auto floor = exact;
    floor.rounding = mdv::modbus::WriteRounding::Floor;
    Require(
        mdv::modbus::EncodePointValue(
            floor,
            mdv::modbus::PointValue(1.1)) == 5U,
        "floor rounding mismatch");

    auto ceil = exact;
    ceil.rounding = mdv::modbus::WriteRounding::Ceil;
    Require(
        mdv::modbus::EncodePointValue(
            ceil,
            mdv::modbus::PointValue(1.1)) == 6U,
        "ceil rounding mismatch");
}

void TestRawRangeChecks()
{
    auto unsignedPoint = NumberPoint();
    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodePointValue(
                    unsignedPoint,
                    mdv::modbus::PointValue(-1.0)));
        },
        "uint16 range");

    auto signedPoint = NumberPoint();
    signedPoint.rawType = mdv::modbus::RawType::Int16;
    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodePointValue(
                    signedPoint,
                    mdv::modbus::PointValue(40000.0)));
        },
        "int16 range");
}

void TestSemanticTypeMismatch()
{
    auto number = NumberPoint();

    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodePointValue(
                    number,
                    mdv::modbus::PointValue(true)));
        },
        "finite numeric");

    mdv::modbus::PointDefinition boolean;
    boolean.type = mdv::modbus::PointType::Boolean;
    RequireConversionError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodePointValue(
                    boolean,
                    mdv::modbus::PointValue(1.0)));
        },
        "boolean semantic");
}

} // namespace

int main()
{
    try {
        TestBooleanConversion();
        TestEnumConversion();
        TestUnsignedNumericScaling();
        TestSignedInt16Conversion();
        TestLimitsAndStep();
        TestExactAndDeclaredRounding();
        TestRawRangeChecks();
        TestSemanticTypeMismatch();

        std::cout << "MDVWB Modbus value conversion tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus value conversion tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
