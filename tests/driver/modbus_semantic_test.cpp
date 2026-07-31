#include "modbus_semantic.h"

#include <cmath>
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
void RequireSemanticError(
    Function&& function,
    std::string_view expectedText)
{
    try {
        function();
    }
    catch (const mdv::modbus::SemanticConversionError& error) {
        if (std::string_view(error.what()).find(expectedText) ==
            std::string_view::npos) {
            throw std::runtime_error(
                "semantic conversion failed for the wrong reason: " +
                std::string(error.what()));
        }
        return;
    }

    throw std::runtime_error(
        "invalid semantic conversion was accepted: " +
        std::string(expectedText));
}

mdv::modbus::RegisterLocation Holding(std::uint16_t address)
{
    return mdv::modbus::RegisterLocation{
        .space = mdv::modbus::RegisterSpace::HoldingRegister,
        .address = address,
    };
}

mdv::modbus::PointDefinition EnumPoint(
    std::uint16_t readAddress,
    std::uint16_t writeAddress,
    std::map<std::uint16_t, std::string> readMap,
    std::map<std::string, std::uint16_t, std::less<>> writeMap)
{
    mdv::modbus::PointDefinition point;
    point.type = mdv::modbus::PointType::Enum;
    point.read = Holding(readAddress);
    point.write = Holding(writeAddress);
    point.enumMappings.read = std::move(readMap);
    point.enumMappings.write = std::move(writeMap);
    return point;
}

mdv::modbus::PointDefinition BooleanPoint(
    std::uint16_t readAddress,
    std::uint16_t writeAddress)
{
    mdv::modbus::PointDefinition point;
    point.type = mdv::modbus::PointType::Boolean;
    point.read = Holding(readAddress);
    point.write = Holding(writeAddress);
    return point;
}

mdv::modbus::PointDefinition NumberPoint(
    std::uint16_t readAddress,
    std::optional<std::uint16_t> writeAddress = std::nullopt)
{
    mdv::modbus::PointDefinition point;
    point.type = mdv::modbus::PointType::Number;
    point.read = Holding(readAddress);
    if (writeAddress.has_value()) {
        point.write = Holding(*writeAddress);
    }
    return point;
}

mdv::modbus::ModbusProfile MakeProfile()
{
    mdv::modbus::ModbusProfile profile;
    profile.id = "semantic_test";
    profile.name = "Semantic Test";
    profile.registerAddressing = "pdu_zero_based";

    profile.capabilities.power = true;
    profile.capabilities.mode = true;
    profile.capabilities.fanSpeed = true;
    profile.capabilities.setTemperature = true;
    profile.capabilities.roomTemperature = true;
    profile.capabilities.alarm = true;
    profile.capabilities.blinds = true;
    profile.capabilities.blocked = true;

    profile.points["power"] = EnumPoint(
        10U, 20U,
        {{0U, "off"}, {1U, "on"}},
        {{"off", 0U}, {"on", 1U}});

    profile.points["mode"] = EnumPoint(
        11U, 21U,
        {{2U, "cool"}, {4U, "dry"}, {8U, "fan"}, {16U, "heat"}},
        {{"cool", 2U}, {"dry", 4U}, {"fan", 8U}, {"heat", 16U}});

    profile.points["fanSpeed"] = EnumPoint(
        12U, 22U,
        {{1U, "auto"}, {2U, "high"}, {4U, "medium"}, {8U, "low"}},
        {{"auto", 1U}, {"high", 2U}, {"medium", 4U}, {"low", 8U}});

    auto setTemperature = NumberPoint(13U, 23U);
    setTemperature.transform = mdv::modbus::NumericTransform{
        .scale = 0.5,
        .offset = 10.0,
    };
    setTemperature.limits = mdv::modbus::NumericLimits{
        .minimum = 16.0,
        .maximum = 30.0,
        .step = 0.5,
    };
    profile.points["setTemperature"] = setTemperature;

    auto roomTemperature = NumberPoint(14U);
    roomTemperature.rawType = mdv::modbus::RawType::Int16;
    roomTemperature.transform = mdv::modbus::NumericTransform{
        .scale = 0.1,
        .offset = 0.0,
    };
    profile.points["roomTemperature"] = roomTemperature;

    profile.points["alarmCode"] = NumberPoint(15U);
    profile.points["blinds"] = BooleanPoint(16U, 26U);
    profile.points["blocked"] = EnumPoint(
        17U, 27U,
        {{0U, "unblocked"}, {9U, "blocked"}},
        {{"unblocked", 0U}, {"blocked", 9U}});

    return profile;
}

void TestSemanticReadsPopulateCommonState()
{
    const auto profile = MakeProfile();
    mdv::DriverDeviceState state;
    state.address = 7;

    mdv::modbus::ApplySemanticRead(state, profile, "power", 1U);
    mdv::modbus::ApplySemanticRead(state, profile, "mode", 16U);
    mdv::modbus::ApplySemanticRead(state, profile, "fanSpeed", 4U);
    mdv::modbus::ApplySemanticRead(state, profile, "setTemperature", 25U);
    mdv::modbus::ApplySemanticRead(state, profile, "roomTemperature", 0xFF85U);
    mdv::modbus::ApplySemanticRead(state, profile, "alarmCode", 7U);
    mdv::modbus::ApplySemanticRead(state, profile, "blinds", 1U);
    mdv::modbus::ApplySemanticRead(state, profile, "blocked", 9U);

    Require(state.power, "Power was not applied to common state");
    Require(state.mode == mdv::HvacMode::Heat,
            "Mode was not normalized to HvacMode");
    Require(state.fanSpeed == mdv::HvacFanSpeed::Medium,
            "FanSpeed was not normalized to HvacFanSpeed");
    Require(state.setTemperature == 22.5,
            "SetTemperature numeric conversion mismatch");
    Require(
        state.roomTemperature.has_value() &&
            std::abs(*state.roomTemperature - (-12.3)) < 1e-12,
        "RoomTemperature signed conversion mismatch");
    Require(state.alarmCode == 7, "AlarmCode conversion mismatch");
    Require(state.blinds == true, "Blinds conversion mismatch");
    Require(state.blocked == true, "Blocked conversion mismatch");
}

void TestSemanticWritesUseCommonDriverValues()
{
    const auto profile = MakeProfile();

    const auto power = mdv::modbus::EncodeSemanticWrite(
        profile,
        mdv::DriverControl::Power,
        mdv::DriverCommandValue(true));
    Require(power.location.address == 20U && power.rawValue == 1U,
            "Power semantic write mismatch");

    const auto mode = mdv::modbus::EncodeSemanticWrite(
        profile,
        mdv::DriverControl::Mode,
        mdv::DriverCommandValue(mdv::HvacMode::Heat));
    Require(mode.location.address == 21U && mode.rawValue == 16U,
            "Mode semantic write mismatch");

    const auto fan = mdv::modbus::EncodeSemanticWrite(
        profile,
        mdv::DriverControl::FanSpeed,
        mdv::DriverCommandValue(mdv::HvacFanSpeed::Low));
    Require(fan.location.address == 22U && fan.rawValue == 8U,
            "FanSpeed semantic write mismatch");

    const auto setTemperature = mdv::modbus::EncodeSemanticWrite(
        profile,
        mdv::DriverControl::SetTemperature,
        mdv::DriverCommandValue(22.5));
    Require(
        setTemperature.location.address == 23U &&
            setTemperature.rawValue == 25U,
        "SetTemperature semantic write mismatch");

    const auto blinds = mdv::modbus::EncodeSemanticWrite(
        profile,
        mdv::DriverControl::Blinds,
        mdv::DriverCommandValue(false));
    Require(blinds.location.address == 26U && blinds.rawValue == 0U,
            "Blinds semantic write mismatch");

    const auto blocked = mdv::modbus::EncodeSemanticWrite(
        profile,
        mdv::DriverControl::Blocked,
        mdv::DriverCommandValue(true));
    Require(blocked.location.address == 27U && blocked.rawValue == 9U,
            "Blocked semantic write mismatch");
}

void TestUnsupportedValuesAreRejectedBeforeBusWrite()
{
    const auto profile = MakeProfile();

    RequireSemanticError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodeSemanticWrite(
                    profile,
                    mdv::DriverControl::Mode,
                    mdv::DriverCommandValue(mdv::HvacMode::Auto)));
        },
        "writeMap");

    RequireSemanticError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodeSemanticWrite(
                    profile,
                    mdv::DriverControl::SetTemperature,
                    mdv::DriverCommandValue(22.25)));
        },
        "configured step");
}

void TestCapabilitiesGateCommonExposure()
{
    auto profile = MakeProfile();
    profile.capabilities.fanSpeed = false;

    Require(!mdv::modbus::IsSemanticPointEnabled(profile, "fanSpeed"),
            "disabled capability was exposed as supported");
    Require(mdv::modbus::IsSemanticPointEnabled(profile, "mode"),
            "enabled capability was hidden");

    mdv::DriverDeviceState state;
    RequireSemanticError(
        [&] {
            mdv::modbus::ApplySemanticRead(
                state, profile, "fanSpeed", 4U);
        },
        "disabled by profile capabilities");

    RequireSemanticError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodeSemanticWrite(
                    profile,
                    mdv::DriverControl::FanSpeed,
                    mdv::DriverCommandValue(mdv::HvacFanSpeed::Medium)));
        },
        "disabled by profile capabilities");
}

void TestReadOnlyAndUnknownPointsAreRejected()
{
    const auto profile = MakeProfile();

    RequireSemanticError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodeSemanticWrite(
                    profile,
                    mdv::DriverControl::SetTemperature,
                    mdv::DriverCommandValue(31.0)));
        },
        "above maximum");

    auto readOnly = profile;
    readOnly.points.at("setTemperature").write.reset();
    RequireSemanticError(
        [&] {
            static_cast<void>(
                mdv::modbus::EncodeSemanticWrite(
                    readOnly,
                    mdv::DriverControl::SetTemperature,
                    mdv::DriverCommandValue(22.5)));
        },
        "not writable");
}

void TestAlarmCodeMustRemainIntegral()
{
    auto profile = MakeProfile();
    profile.points.at("alarmCode").transform =
        mdv::modbus::NumericTransform{
            .scale = 0.5,
            .offset = 0.0,
        };

    mdv::DriverDeviceState state;
    RequireSemanticError(
        [&] {
            mdv::modbus::ApplySemanticRead(
                state, profile, "alarmCode", 3U);
        },
        "whole number");
}

} // namespace

int main()
{
    try {
        TestSemanticReadsPopulateCommonState();
        TestSemanticWritesUseCommonDriverValues();
        TestUnsupportedValuesAreRejectedBeforeBusWrite();
        TestCapabilitiesGateCommonExposure();
        TestReadOnlyAndUnknownPointsAreRejected();
        TestAlarmCodeMustRemainIntegral();

        std::cout << "MDVWB Modbus semantic conversion tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus semantic conversion tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
