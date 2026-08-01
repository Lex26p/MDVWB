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

mdv::modbus::ModbusProfile AdjacentAndSharedProfile()
{
    auto profile = ProductionProfile();
    profile.capabilities.roomTemperature = true;

    mdv::modbus::PointDefinition room;
    room.type = mdv::modbus::PointType::Number;
    room.rawType = mdv::modbus::RawType::UInt16;
    room.read = mdv::modbus::RegisterLocation{
        .space = mdv::modbus::RegisterSpace::HoldingRegister,
        .address = 40029U,
        .reference = std::nullopt,
    };
    room.transform = mdv::modbus::NumericTransform{
        .scale = 0.1,
        .offset = 0.0,
    };
    profile.points["roomTemperature"] = room;

    profile.points.at("alarmCode").read->address = 40028U;
    profile.points.at("alarmCode").read->reference.reset();
    return profile;
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
    Require(
        plan.metrics.optimizedSemanticTransactionsPerCycle == 4U &&
            plan.metrics.optimizedTotalTransactionsPerCycle == 6U,
        "production profile was unsafely batched across gaps");
    Require(
        plan.metrics.optimizedRegistersRequestedPerCycle == 6U &&
            plan.metrics.reusedSemanticReadsPerCycle == 0U &&
            plan.metrics.savedTransactionsPerCycle == 0U,
        "production optimization metrics are wrong");

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
    Require(
        first.semanticBatches.size() == 2U &&
            first.semanticBatches[0].startAddress == 40028U &&
            first.semanticBatches[0].quantity == 1U &&
            first.semanticBatches[1].startAddress == 40035U &&
            first.semanticBatches[1].quantity == 1U,
        "non-adjacent production registers were combined");

    const auto& second = plan.devices[1];
    Require(second.logicalAddress == 2U, "second logical address changed");
    Require(second.probe.address == 40130U, "probe stride was not resolved");
    Require(second.semanticReads[0].location.address == 40119U, "Power stride is wrong");
    Require(second.semanticReads[1].location.address == 40126U, "Alarm stride is wrong");
    Require(
        second.powerRead.has_value() && second.powerRead->address == 40119U,
        "second Power confirmation location was not cached");
}

void TestAdjacentReadsAreBatchedAndSharedRegistersReused()
{
    const auto profile = AdjacentAndSharedProfile();
    const auto plan = mdv::modbus::BuildModbusPollPlan(profile, {1U});

    Require(plan.devices.size() == 1U, "optimized plan lost its device");
    const auto& device = plan.devices.front();
    Require(device.semanticReads.size() == 3U, "semantic points were lost");
    Require(device.semanticBatches.size() == 1U, "adjacent reads were not batched");
    Require(
        device.semanticBatches[0].slaveId == 1U &&
            device.semanticBatches[0].startAddress == 40028U &&
            device.semanticBatches[0].quantity == 2U,
        "optimized FC03 span is wrong");

    Require(
        device.semanticReads[0].pointName == "power" &&
            device.semanticReads[0].batchIndex == 0U &&
            device.semanticReads[0].registerOffset == 0U,
        "Power batch placement is wrong");
    Require(
        device.semanticReads[1].pointName == "roomTemperature" &&
            device.semanticReads[1].registerOffset == 1U,
        "adjacent room-temperature placement is wrong");
    Require(
        device.semanticReads[2].pointName == "alarmCode" &&
            device.semanticReads[2].registerOffset == 0U,
        "shared register was not reused");

    Require(
        plan.metrics.semanticTransactionsPerCycle == 3U &&
            plan.metrics.totalTransactionsPerCycle == 4U &&
            plan.metrics.registersRequestedPerCycle == 4U,
        "optimized test baseline is wrong");
    Require(
        plan.metrics.optimizedSemanticTransactionsPerCycle == 1U &&
            plan.metrics.optimizedTotalTransactionsPerCycle == 2U &&
            plan.metrics.optimizedRegistersRequestedPerCycle == 3U,
        "optimized traffic metrics are wrong");
    Require(
        plan.metrics.reusedSemanticReadsPerCycle == 1U &&
            plan.metrics.savedTransactionsPerCycle == 2U,
        "optimization savings are wrong");
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
        TestAdjacentReadsAreBatchedAndSharedRegistersReused();
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
