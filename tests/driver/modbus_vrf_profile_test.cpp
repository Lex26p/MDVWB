#include "modbus_profile.h"
#include "modbus_resolver.h"
#include "modbus_scan.h"
#include "modbus_scan_execute.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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

template <typename Function>
void RequireProfileError(
    Function&& function,
    std::string_view expected)
{
    try {
        function();
    }
    catch (const mdv::modbus::ProfileError& error) {
        if (std::string_view(error.what()).find(expected) ==
            std::string_view::npos) {
            throw std::runtime_error(
                "profile failed for the wrong reason: " +
                std::string(error.what()));
        }
        return;
    }

    throw std::runtime_error(
        "invalid profile was accepted: " +
        std::string(expected));
}

std::filesystem::path ProfilePath()
{
    return std::filesystem::path(MDVWB_SOURCE_DIR) /
        "profiles/modbus/vrf_add_controller.json";
}

const mdv::modbus::PointDefinition& Point(
    const mdv::modbus::ModbusProfile& profile,
    std::string_view name)
{
    const auto iterator = profile.points.find(name);
    if (iterator == profile.points.end()) {
        throw std::runtime_error(
            "production profile is missing point " +
            std::string(name));
    }
    return iterator->second;
}

void TestProductionProfileLoads()
{
    const auto profile =
        mdv::modbus::LoadProfileFile(ProfilePath());

    Require(profile.id == "vrf_add_controller", "profile id mismatch");
    Require(profile.name == "VRF Add Controller", "profile name mismatch");
    Require(
        profile.registerAddressing == "pdu_zero_based",
        "profile does not store literal PDU addresses");

    Require(profile.transport.baudRate == 9600U, "baud rate mismatch");
    Require(profile.transport.dataBits == 8U, "data bits mismatch");
    Require(
        profile.transport.parity == mdv::SerialParity::None,
        "parity mismatch");
    Require(profile.transport.stopBits == 1U, "stop bits mismatch");

    const auto* addressing =
        std::get_if<mdv::modbus::FixedSlaveStrideAddressing>(
            &profile.addressing);
    Require(addressing != nullptr, "addressing type mismatch");
    Require(addressing->logicalMin == 1U, "logicalMin mismatch");
    Require(addressing->logicalMax == 63U, "logicalMax mismatch");
    Require(addressing->slaveId == 1U, "Slave ID mismatch");
    Require(
        addressing->firstLogicalAddress == 1U,
        "first logical address mismatch");
    Require(addressing->registerStride == 91U, "register stride mismatch");

    Require(profile.capabilities.power, "Power must be enabled");
    Require(profile.capabilities.alarm, "Alarm must be enabled");
    Require(!profile.capabilities.mode, "Mode must remain disabled");
    Require(!profile.capabilities.fanSpeed, "FanSpeed must remain disabled");
    Require(
        !profile.capabilities.setTemperature,
        "SetTemperature must remain disabled");
    Require(
        !profile.capabilities.roomTemperature,
        "RoomTemperature must remain disabled");
    Require(!profile.capabilities.blinds, "Blinds must remain disabled");
    Require(!profile.capabilities.blocked, "Blocked must remain disabled");

    Require(
        profile.probe.read.space ==
            mdv::modbus::RegisterSpace::HoldingRegister,
        "probe data space mismatch");
    Require(profile.probe.read.address == 40039U, "probe address mismatch");
    Require(profile.probe.quantity == 1U, "probe quantity mismatch");
    Require(
        profile.probe.presence == mdv::modbus::ProbePresence::AnyNonZero,
        "probe presence rule mismatch");

    const auto& power = Point(profile, "power");
    Require(
        power.type == mdv::modbus::PointType::Boolean,
        "Power point type mismatch");
    Require(power.read.has_value(), "Power read location missing");
    Require(power.write.has_value(), "Power write location missing");
    Require(power.read->address == 40028U, "Power read address mismatch");
    Require(power.write->address == 40078U, "Power write address mismatch");

    const auto& alarm = Point(profile, "alarmCode");
    Require(
        alarm.type == mdv::modbus::PointType::Number,
        "Alarm point type mismatch");
    Require(alarm.read.has_value(), "Alarm read location missing");
    Require(alarm.read->address == 40035U, "Alarm address mismatch");
}

void TestResolvedRegistersUseLiteralSourceAddresses()
{
    const auto profile =
        mdv::modbus::LoadProfileFile(ProfilePath());

    const auto plan = mdv::modbus::BuildScanPlan(profile);

    Require(plan.size() == 63U, "scan plan must contain 63 candidates");

    Require(plan[0].probe.has_value(), "logical 1 probe missing");
    Require(plan[0].probe->slaveId == 1U, "logical 1 Slave ID mismatch");
    Require(plan[0].probe->address == 40039U, "logical 1 probe mismatch");
    Require(
        plan[0].probe->presence ==
            mdv::modbus::ProbePresence::AnyNonZero,
        "logical 1 presence rule mismatch");

    Require(plan[1].probe.has_value(), "logical 2 probe missing");
    Require(
        plan[1].probe->address == 40130U,
        "logical 2 probe did not apply +91 stride");

    Require(plan[62].probe.has_value(), "logical 63 probe missing");
    Require(
        plan[62].probe->address == 45681U,
        "logical 63 probe register mismatch");

    const auto& power = Point(profile, "power");
    const auto logical2PowerRead = mdv::modbus::ResolveRegisterLocation(
        profile,
        2U,
        *power.read);
    const auto logical2PowerWrite = mdv::modbus::ResolveRegisterLocation(
        profile,
        2U,
        *power.write);

    Require(logical2PowerRead.has_value(), "logical 2 Power read unresolved");
    Require(
        logical2PowerRead->address == 40119U,
        "logical 2 Power read register mismatch");
    Require(logical2PowerWrite.has_value(), "logical 2 Power write unresolved");
    Require(
        logical2PowerWrite->address == 40169U,
        "logical 2 Power write register mismatch");
}

class TemperatureProbeTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        ++requestCount;

        if (request.size() != 8U) {
            throw std::runtime_error("unexpected probe request size");
        }

        const auto startAddress = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(request[2]) << 8U) |
            static_cast<std::uint16_t>(request[3]));

        const auto quantity = static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(request[4]) << 8U) |
            static_cast<std::uint16_t>(request[5]));

        mdv::modbus::ParsedResponse response;
        response.status = mdv::modbus::ResponseStatus::Success;
        response.slaveId = request[0];
        response.function =
            mdv::modbus::Function::ReadHoldingRegisters;
        response.registers.assign(quantity, 0U);

        // Live installation rule chosen for this profile:
        // absent indoor unit -> inlet temperature register returns zero.
        // Simulate logical 2 as present with a non-zero raw temperature.
        if (startAddress == 40130U) {
            response.registers[0] = 24U;
        }

        mdv::modbus::TransactionResult result;
        result.status = mdv::modbus::TransactionStatus::Success;
        result.response = std::move(response);
        return result;
    }

    std::size_t requestCount = 0;
};

void TestTemperatureZeroPresenceRule()
{
    const auto profile =
        mdv::modbus::LoadProfileFile(ProfilePath());

    TemperatureProbeTransport transport;
    const auto report =
        mdv::modbus::ExecuteProfileScan(profile, transport);

    Require(
        transport.requestCount == 63U,
        "production profile scan must issue 63 read-only probes");

    Require(
        report[0].disposition == mdv::modbus::ScanDisposition::NotFound,
        "zero temperature must classify logical 1 as NotFound");
    Require(
        report[0].reason == mdv::modbus::ScanReason::PresenceMismatch,
        "zero temperature must use PresenceMismatch reason");

    Require(
        report[1].disposition == mdv::modbus::ScanDisposition::Found,
        "non-zero temperature must classify logical 2 as Found");
    Require(
        report[1].reason == mdv::modbus::ScanReason::Success,
        "non-zero temperature must use Success reason");

    for (std::size_t index = 2U; index < report.size(); ++index) {
        Require(
            report[index].disposition ==
                mdv::modbus::ScanDisposition::NotFound,
            "zero temperature candidate was not classified NotFound");
        Require(
            report[index].reason ==
                mdv::modbus::ScanReason::PresenceMismatch,
            "zero temperature candidate reason mismatch");
    }
}

void TestInvalidProbePresenceRejected()
{
    auto jsonText = std::string{
        R"json({
          "schemaVersion": 1,
          "id": "invalid_probe_presence",
          "name": "Invalid Probe Presence",
          "registerAddressing": "pdu_zero_based",
          "transport": {
            "baudRate": 9600,
            "dataBits": 8,
            "parity": "none",
            "stopBits": 1
          },
          "addressing": {
            "type": "fixed_slave_stride",
            "logicalMin": 1,
            "logicalMax": 1,
            "slaveId": 1,
            "firstLogicalAddress": 1,
            "registerStride": 91
          },
          "capabilities": {
            "power": true
          },
          "probe": {
            "read": {
              "space": "holding_register",
              "address": 40039
            },
            "quantity": 1,
            "presence": "guess_something"
          },
          "points": {
            "power": {
              "type": "boolean",
              "read": {
                "space": "holding_register",
                "address": 40028
              }
            }
          }
        })json"};

    RequireProfileError(
        [&] {
            static_cast<void>(mdv::modbus::ParseProfile(jsonText));
        },
        "any_response, any_nonzero");
}

} // namespace

int main()
{
    try {
        TestProductionProfileLoads();
        TestResolvedRegistersUseLiteralSourceAddresses();
        TestTemperatureZeroPresenceRule();
        TestInvalidProbePresenceRejected();

        std::cout << "MDVWB VRF production profile tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB VRF production profile tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
