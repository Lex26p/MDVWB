#include "modbus_poll_plan.h"

#include "modbus_profile.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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
void RequireInvalid(Function&& function, std::string_view message)
{
    try {
        function();
    }
    catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

mdv::modbus::ModbusProfile ProductionProfile()
{
    return mdv::modbus::LoadProfileFile(
        std::filesystem::path(MDVWB_SOURCE_DIR) /
        "profiles/modbus/vrf_add_controller.json");
}

void TestProductionBaselineAndResolvedLocations()
{
    const auto profile = ProductionProfile();
    const auto plan = mdv::modbus::BuildModbusPollPlan(profile, {1U, 2U});

    Require(plan.devices.size() == 2U, "poll plan lost configured devices");
    Require(plan.metrics.deviceCount == 2U, "device baseline is wrong");
    Require(
        plan.metrics.probeTransactionsPerCycle == 2U,
        "probe transaction baseline is wrong");
    Require(
        plan.metrics.semanticTransactionsPerCycle == 4U,
        "semantic transaction baseline is wrong");
    Require(
        plan.metrics.totalTransactionsPerCycle == 6U,
        "total transaction baseline is wrong");
    Require(
        plan.metrics.registersRequestedPerCycle == 6U,
        "register baseline is wrong");

    const auto& first = plan.devices[0];
    Require(first.logicalAddress == 1U, "first logical address changed");
    Require(first.probe.slaveId == 1U, "first probe slave is wrong");
    Require(first.probe.address == 40039U, "first probe address is wrong");
    Require(first.semanticReads.size() == 2U, "first semantic read count is wrong");
    Require(first.semanticReads[0].pointName == "power", "semantic order is unstable");
    Require(first.semanticReads[0].location.address == 40028U, "Power read is wrong");
    Require(first.semanticReads[1].pointName == "alarmCode", "Alarm order is unstable");
    Require(first.semanticReads[1].location.address == 40035U, "Alarm read is wrong");
    Require(
        first.powerRead.has_value() && first.powerRead->address == 40028U,
        "Power confirmation location was not cached");

    const auto& second = plan.devices[1];
    Require(second.logicalAddress == 2U, "second logical address changed");
    Require(second.probe.address == 40130U, "probe stride was not resolved");
    Require(second.semanticReads[0].location.address == 40119U, "Power stride is wrong");
    Require(second.semanticReads[1].location.address == 40126U, "Alarm stride is wrong");
    Require(
        second.powerRead.has_value() && second.powerRead->address == 40119U,
        "second Power confirmation location was not cached");
}

void TestValidationRemainsTrafficIndependent()
{
    const auto profile = ProductionProfile();

    RequireInvalid(
        [&] { static_cast<void>(mdv::modbus::BuildModbusPollPlan(profile, {})); },
        "empty device list was accepted");
    RequireInvalid(
        [&] { static_cast<void>(mdv::modbus::BuildModbusPollPlan(profile, {0U})); },
        "logical address zero was accepted");
    RequireInvalid(
        [&] { static_cast<void>(mdv::modbus::BuildModbusPollPlan(profile, {1U, 1U})); },
        "duplicate logical address was accepted");

    auto unsupported = profile;
    unsupported.points.at("power").read->space =
        mdv::modbus::RegisterSpace::InputRegister;
    RequireInvalid(
        [&] {
            static_cast<void>(
                mdv::modbus::BuildModbusPollPlan(unsupported, {1U}));
        },
        "unsupported semantic data space was accepted");
}

} // namespace

int main()
{
    try {
        TestProductionBaselineAndResolvedLocations();
        TestValidationRemainsTrafficIndependent();
        std::cout << "MDVWB Modbus poll plan tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus poll plan tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
