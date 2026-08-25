#include "modbus_driver.h"
#include "modbus_profile.h"
#include "modbus_resolver.h"
#include "modbus_scan.h"
#include "modbus_semantic.h"
#include "modbus_value.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#ifndef MDVWB_SOURCE_DIR
#error MDVWB_SOURCE_DIR must point to the repository source directory
#endif

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::filesystem::path ProfilePath()
{
    return std::filesystem::path(MDVWB_SOURCE_DIR) /
        "profiles/modbus/thermostat.json";
}

const mdv::modbus::PointDefinition& Point(
    const mdv::modbus::ModbusProfile& profile,
    std::string_view name)
{
    const auto iterator = profile.points.find(name);
    if (iterator == profile.points.end()) {
        throw std::runtime_error(
            "Thermostat profile is missing point " + std::string(name));
    }
    return iterator->second;
}

void RequireReadWriteAddress(
    const mdv::modbus::PointDefinition& point,
    std::uint16_t expected,
    std::string_view name)
{
    Require(point.read.has_value(), std::string(name) + " read is missing");
    Require(point.write.has_value(), std::string(name) + " write is missing");
    Require(
        point.read->space == mdv::modbus::RegisterSpace::HoldingRegister &&
            point.write->space == mdv::modbus::RegisterSpace::HoldingRegister,
        std::string(name) + " must use holding registers");
    Require(
        point.read->address == expected && point.write->address == expected,
        std::string(name) + " register address mismatch");
    Require(
        point.write->writeFunction ==
            mdv::modbus::WriteFunction::WriteSingleRegister,
        std::string(name) + " must use FC06");
}

void TestProfileFacts()
{
    const auto profile = mdv::modbus::LoadProfileFile(ProfilePath());

    Require(profile.id == "thermostat", "profile id mismatch");
    Require(profile.name == "Thermostat", "profile name mismatch");
    Require(
        profile.registerAddressing == "pdu_zero_based",
        "profile must use literal PDU addresses");
    Require(profile.transport.baudRate == 9600U, "baud rate mismatch");
    Require(profile.transport.dataBits == 8U, "data bits mismatch");
    Require(
        profile.transport.parity == mdv::SerialParity::None,
        "parity mismatch");
    Require(profile.transport.stopBits == 1U, "stop bits mismatch");

    const auto* addressing =
        std::get_if<mdv::modbus::DirectSlaveAddressing>(
            &profile.addressing);
    Require(addressing != nullptr, "addressing type mismatch");
    Require(addressing->logicalMin == 1U, "logical minimum mismatch");
    Require(addressing->logicalMax == 63U, "logical maximum mismatch");
    Require(addressing->registerOffset == 0U, "register offset mismatch");

    Require(profile.capabilities.power, "Power must be enabled");
    Require(profile.capabilities.mode, "Mode must be enabled");
    Require(profile.capabilities.fanSpeed, "FanSpeed must be enabled");
    Require(
        profile.capabilities.setTemperature,
        "SetTemperature must be enabled");
    Require(
        profile.capabilities.roomTemperature,
        "RoomTemperature must be enabled");
    Require(!profile.capabilities.alarm, "Alarm must remain disabled");
    Require(!profile.capabilities.blinds, "Blinds must remain disabled");
    Require(!profile.capabilities.blocked, "Blocked must remain disabled");

    Require(
        profile.probe.read.space ==
            mdv::modbus::RegisterSpace::HoldingRegister,
        "probe must read a holding register");
    Require(profile.probe.read.address == 0x2060U, "probe address mismatch");
    Require(profile.probe.quantity == 1U, "probe quantity mismatch");
    Require(
        profile.probe.presence == mdv::modbus::ProbePresence::AnyResponse,
        "probe presence rule mismatch");

    const auto& power = Point(profile, "power");
    Require(power.type == mdv::modbus::PointType::Enum,
            "Power must use the documented 1/2 enum");
    RequireReadWriteAddress(power, 0x2060U, "Power");
    Require(
        power.enumMappings.read.at(1U) == "off" &&
            power.enumMappings.read.at(2U) == "on" &&
            power.enumMappings.write.at("off") == 1U &&
            power.enumMappings.write.at("on") == 2U,
        "Power mapping mismatch");

    const auto& fanSpeed = Point(profile, "fanSpeed");
    RequireReadWriteAddress(fanSpeed, 0x2061U, "FanSpeed");
    Require(
        fanSpeed.enumMappings.read.at(1U) == "low" &&
            fanSpeed.enumMappings.read.at(2U) == "medium" &&
            fanSpeed.enumMappings.read.at(3U) == "high" &&
            fanSpeed.enumMappings.read.at(4U) == "auto",
        "FanSpeed read mapping mismatch");
    Require(
        fanSpeed.enumMappings.write.at("low") == 1U &&
            fanSpeed.enumMappings.write.at("medium") == 2U &&
            fanSpeed.enumMappings.write.at("high") == 3U &&
            fanSpeed.enumMappings.write.at("auto") == 4U,
        "FanSpeed write mapping mismatch");

    const auto& mode = Point(profile, "mode");
    RequireReadWriteAddress(mode, 0x2062U, "Mode");
    Require(
        mode.enumMappings.read.at(1U) == "cool" &&
            mode.enumMappings.read.at(2U) == "heat" &&
            mode.enumMappings.write.at("cool") == 1U &&
            mode.enumMappings.write.at("heat") == 2U,
        "Mode mapping mismatch");

    const auto& roomTemperature = Point(profile, "roomTemperature");
    Require(
        roomTemperature.type == mdv::modbus::PointType::Number,
        "RoomTemperature type mismatch");
    Require(
        roomTemperature.read.has_value() &&
            roomTemperature.read->address == 0x2064U,
        "RoomTemperature must use the integer register");
    Require(
        !roomTemperature.write.has_value(),
        "RoomTemperature must remain read-only");
    Require(
        roomTemperature.transform.has_value() &&
            roomTemperature.transform->scale == 1.0 &&
            roomTemperature.transform->offset == 0.0,
        "RoomTemperature transform mismatch");

    const auto& setTemperature = Point(profile, "setTemperature");
    RequireReadWriteAddress(setTemperature, 0x2065U, "SetTemperature");
    Require(
        setTemperature.transform.has_value() &&
            setTemperature.transform->scale == 0.1 &&
            setTemperature.transform->offset == 0.0,
        "SetTemperature transform mismatch");
    Require(
        setTemperature.limits.has_value() &&
            setTemperature.limits->minimum == 16.0 &&
            setTemperature.limits->maximum == 34.0 &&
            setTemperature.limits->step == 1.0,
        "SetTemperature limits mismatch");

    Require(
        profile.points.find("alarmCode") == profile.points.end(),
        "undocumented Alarm register was added");
}

void TestDirectSlaveResolutionAndScan()
{
    const auto profile = mdv::modbus::LoadProfileFile(ProfilePath());
    const auto plan = mdv::modbus::BuildScanPlan(profile);

    Require(plan.size() == 63U, "scan plan must contain 63 candidates");
    for (std::size_t index = 0; index < plan.size(); ++index) {
        const auto expectedSlave = static_cast<std::uint8_t>(index + 1U);
        Require(plan[index].probe.has_value(), "scan probe is missing");
        Require(
            plan[index].probe->slaveId == expectedSlave,
            "logical address did not map to the same Slave ID");
        Require(
            plan[index].probe->address == 0x2060U,
            "scan probe applied an unexpected address offset");
    }

    const auto& setTemperature = Point(profile, "setTemperature");
    const auto resolved = mdv::modbus::ResolveRegisterLocation(
        profile,
        18U,
        *setTemperature.write);
    Require(resolved.has_value(), "Slave 18 setpoint did not resolve");
    Require(resolved->slaveId == 18U, "resolved Slave ID mismatch");
    Require(resolved->address == 0x2065U,
            "resolved setpoint address mismatch");
}

void TestSemanticReadAndWriteMappings()
{
    const auto profile = mdv::modbus::LoadProfileFile(ProfilePath());
    mdv::DriverDeviceState state;
    state.address = 18U;

    mdv::modbus::ApplySemanticRead(state, profile, "power", 2U);
    mdv::modbus::ApplySemanticRead(state, profile, "mode", 1U);
    mdv::modbus::ApplySemanticRead(state, profile, "fanSpeed", 3U);
    mdv::modbus::ApplySemanticRead(
        state, profile, "roomTemperature", 23U);
    mdv::modbus::ApplySemanticRead(
        state, profile, "setTemperature", 210U);

    Require(state.power, "Power read mapping mismatch");
    Require(state.mode == mdv::HvacMode::Cool, "Mode read mapping mismatch");
    Require(
        state.fanSpeed == mdv::HvacFanSpeed::High,
        "FanSpeed read mapping mismatch");
    Require(
        state.roomTemperature.has_value() &&
            *state.roomTemperature == 23.0,
        "RoomTemperature read mapping mismatch");
    Require(
        state.setTemperature.has_value() &&
            std::abs(*state.setTemperature - 21.0) < 1e-12,
        "SetTemperature read conversion mismatch");

    const auto power = mdv::modbus::EncodeSemanticWrite(
        profile,
        mdv::DriverControl::Power,
        mdv::DriverCommandValue(false));
    Require(
        power.location.address == 0x2060U && power.rawValue == 1U,
        "Power write mapping mismatch");

    const auto mode = mdv::modbus::EncodeSemanticWrite(
        profile,
        mdv::DriverControl::Mode,
        mdv::DriverCommandValue(mdv::HvacMode::Heat));
    Require(
        mode.location.address == 0x2062U && mode.rawValue == 2U,
        "Mode write mapping mismatch");

    const auto fan = mdv::modbus::EncodeSemanticWrite(
        profile,
        mdv::DriverControl::FanSpeed,
        mdv::DriverCommandValue(mdv::HvacFanSpeed::Auto));
    Require(
        fan.location.address == 0x2061U && fan.rawValue == 4U,
        "FanSpeed write mapping mismatch");

    const auto setTemperature = mdv::modbus::EncodeSemanticWrite(
        profile,
        mdv::DriverControl::SetTemperature,
        mdv::DriverCommandValue(21.0));
    Require(
        setTemperature.location.address == 0x2065U &&
            setTemperature.rawValue == 210U,
        "SetTemperature write conversion mismatch");
}

[[nodiscard]] std::uint16_t RequestU16(
    const mdv::modbus::RtuAdu& request,
    std::size_t offset)
{
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(request.at(offset)) << 8U) |
        static_cast<std::uint16_t>(request.at(offset + 1U)));
}

class ThermostatTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        const auto function = request.at(1);
        const auto address = RequestU16(request, 2U);

        if (function == static_cast<std::uint8_t>(
                mdv::modbus::Function::ReadHoldingRegisters)) {
            const auto quantity = RequestU16(request, 4U);
            mdv::modbus::ParsedResponse response;
            response.status = mdv::modbus::ResponseStatus::Success;
            response.slaveId = request.at(0);
            response.function =
                mdv::modbus::Function::ReadHoldingRegisters;
            response.registers.reserve(quantity);
            for (std::uint16_t offset = 0; offset < quantity; ++offset) {
                response.registers.push_back(ValueAt(
                    static_cast<std::uint16_t>(address + offset)));
            }
            return Success(std::move(response));
        }

        if (function == static_cast<std::uint8_t>(
                mdv::modbus::Function::WriteSingleRegister)) {
            Require(request.size() == 8U, "FC06 request size mismatch");
            const auto value = RequestU16(request, 4U);
            Require(address == 0x2065U, "unexpected FC06 write address");
            setTemperatureRaw = value;
            ++writeCount;

            mdv::modbus::ParsedResponse response;
            response.status = mdv::modbus::ResponseStatus::Success;
            response.slaveId = request.at(0);
            response.function =
                mdv::modbus::Function::WriteSingleRegister;
            response.startAddress = address;
            response.value = value;
            return Success(std::move(response));
        }

        throw std::runtime_error("unexpected Thermostat Modbus function");
    }

    std::vector<mdv::modbus::RtuAdu> requests;
    std::uint16_t setTemperatureRaw = 210U;
    std::size_t writeCount = 0U;

private:
    [[nodiscard]] std::uint16_t ValueAt(std::uint16_t address) const
    {
        switch (address) {
        case 0x2060U: return 2U;
        case 0x2061U: return 3U;
        case 0x2062U: return 1U;
        case 0x2063U: return 230U;
        case 0x2064U: return 23U;
        case 0x2065U: return setTemperatureRaw;
        default:
            throw std::runtime_error(
                "unexpected Thermostat read register");
        }
    }

    [[nodiscard]] static mdv::modbus::TransactionResult Success(
        mdv::modbus::ParsedResponse response)
    {
        mdv::modbus::TransactionResult result;
        result.status = mdv::modbus::TransactionStatus::Success;
        result.response = std::move(response);
        return result;
    }
};

void TestDriverUsesFc06AndConfirmsWithFc03()
{
    const auto profile = mdv::modbus::LoadProfileFile(ProfilePath());
    ThermostatTransport transport;
    mdv::modbus::ModbusDriver driver({18U}, profile, transport);

    const auto poll = driver.ProcessNext();
    Require(
        poll.operation == mdv::DriverOperation::PollRead &&
            poll.outcome == mdv::DriverOutcome::Success,
        "initial Thermostat poll failed");

    driver.ApplyCommand({
        .address = 18U,
        .control = mdv::DriverControl::SetTemperature,
        .value = 22.0,
    });

    const auto write = driver.ProcessNext();
    Require(
        write.operation == mdv::DriverOperation::SetState &&
            write.outcome == mdv::DriverOutcome::Success,
        "Thermostat FC06 write failed");
    Require(transport.writeCount == 1U, "FC06 write count mismatch");
    Require(
        transport.requests.back().at(1) == 0x06U &&
            RequestU16(transport.requests.back(), 2U) == 0x2065U &&
            RequestU16(transport.requests.back(), 4U) == 220U,
        "Thermostat driver did not send the declared FC06 request");

    const auto confirmation = driver.ProcessNext();
    Require(
        confirmation.operation == mdv::DriverOperation::ConfirmRead &&
            confirmation.outcome == mdv::DriverOutcome::Success,
        "Thermostat FC03 confirmation failed");
    Require(
        transport.requests.back().at(1) == 0x03U &&
            RequestU16(transport.requests.back(), 2U) == 0x2065U,
        "Thermostat confirmation did not read the setpoint register");

    const auto state = driver.DeviceStateByAddress(18U);
    Require(
        state.setTemperature.has_value() &&
            std::abs(*state.setTemperature - 22.0) < 1e-12,
        "Thermostat confirmed setpoint was not applied");
}

} // namespace

int main()
{
    try {
        TestProfileFacts();
        TestDirectSlaveResolutionAndScan();
        TestSemanticReadAndWriteMappings();
        TestDriverUsesFc06AndConfirmsWithFc03();

        std::cout << "MDVWB Thermostat production profile tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Thermostat production profile tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
