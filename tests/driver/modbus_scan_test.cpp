#include "modbus_scan.h"

#include <cstdint>
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
void RequireScanPlanError(
    Function&& function,
    std::string_view expectedText)
{
    try {
        function();
    }
    catch (const mdv::modbus::ScanPlanError& error) {
        if (std::string_view(error.what()).find(expectedText) ==
            std::string_view::npos) {
            throw std::runtime_error(
                "scan planning failed for the wrong reason: " +
                std::string(error.what()));
        }
        return;
    }

    throw std::runtime_error(
        "invalid scan plan was accepted: " +
        std::string(expectedText));
}

mdv::modbus::ModbusProfile BaseProfile()
{
    mdv::modbus::ModbusProfile profile;
    profile.id = "scan_test";
    profile.name = "Scan Test";
    profile.registerAddressing = "pdu_zero_based";
    profile.probe.read = mdv::modbus::RegisterLocation{
        .space = mdv::modbus::RegisterSpace::HoldingRegister,
        .address = 100U,
    };
    profile.probe.quantity = 2U;
    return profile;
}

void TestDirectSlavePlanCoversEveryLogicalAddress()
{
    auto profile = BaseProfile();
    profile.addressing = mdv::modbus::DirectSlaveAddressing{
        .logicalMin = 1U,
        .logicalMax = 63U,
        .registerOffset = 3U,
    };

    const auto plan = mdv::modbus::BuildScanPlan(profile);

    Require(plan.size() == 63U, "scan plan does not contain 63 candidates");

    for (std::size_t index = 0; index < plan.size(); ++index) {
        const auto expected = static_cast<std::uint8_t>(index + 1U);
        Require(
            plan[index].logicalAddress == expected,
            "scan plan logical order is not deterministic");
        Require(
            plan[index].Supported(),
            "direct_slave unexpectedly marked a candidate unsupported");
        Require(
            plan[index].probe->slaveId == expected,
            "direct_slave did not map logical address to Slave ID");
        Require(
            plan[index].probe->address == 103U,
            "direct_slave register offset mismatch");
        Require(
            plan[index].probe->quantity == 2U,
            "probe quantity mismatch");
    }
}

void TestFixedStridePlanUsesResolvedOffsets()
{
    auto profile = BaseProfile();
    profile.probe.read.address = 25U;
    profile.probe.quantity = 1U;
    profile.addressing = mdv::modbus::FixedSlaveStrideAddressing{
        .logicalMin = 1U,
        .logicalMax = 3U,
        .slaveId = 7U,
        .firstLogicalAddress = 1U,
        .registerStride = 91U,
    };

    const auto plan = mdv::modbus::BuildScanPlan(profile);

    Require(plan[0].Supported(), "logical 1 should be supported");
    Require(plan[1].Supported(), "logical 2 should be supported");
    Require(plan[2].Supported(), "logical 3 should be supported");
    Require(!plan[3].Supported(), "logical 4 should be unsupported");

    Require(
        plan[0].probe->slaveId == 7U &&
            plan[0].probe->address == 25U,
        "fixed stride logical 1 mismatch");
    Require(
        plan[1].probe->slaveId == 7U &&
            plan[1].probe->address == 116U,
        "fixed stride logical 2 mismatch");
    Require(
        plan[2].probe->slaveId == 7U &&
            plan[2].probe->address == 207U,
        "fixed stride logical 3 mismatch");

    for (std::size_t index = 3U; index < plan.size(); ++index) {
        Require(
            !plan[index].Supported(),
            "candidate outside fixed-stride profile range was not skipped");
    }
}

void TestExplicitPlanPreservesUnsupportedCandidates()
{
    auto profile = BaseProfile();

    mdv::modbus::ExplicitAddressing addressing;
    addressing.logicalMin = 1U;
    addressing.logicalMax = 63U;
    addressing.devices.emplace(
        1U,
        mdv::modbus::ExplicitDeviceLocation{
            .slaveId = 1U,
            .registerOffset = 0U,
        });
    addressing.devices.emplace(
        3U,
        mdv::modbus::ExplicitDeviceLocation{
            .slaveId = 9U,
            .registerOffset = 10U,
        });
    addressing.devices.emplace(
        63U,
        mdv::modbus::ExplicitDeviceLocation{
            .slaveId = 20U,
            .registerOffset = 100U,
        });
    profile.addressing = addressing;

    const auto plan = mdv::modbus::BuildScanPlan(profile);

    Require(plan[0].Supported(), "explicit logical 1 missing");
    Require(!plan[1].Supported(), "explicit logical 2 should be unsupported");
    Require(plan[2].Supported(), "explicit logical 3 missing");
    Require(plan[62].Supported(), "explicit logical 63 missing");

    Require(
        plan[2].probe->slaveId == 9U &&
            plan[2].probe->address == 110U,
        "explicit logical 3 physical mapping mismatch");
    Require(
        plan[62].probe->slaveId == 20U &&
            plan[62].probe->address == 200U,
        "explicit logical 63 physical mapping mismatch");
}

void TestProbeDataSpaceIsPreserved()
{
    auto profile = BaseProfile();
    profile.probe.read.space =
        mdv::modbus::RegisterSpace::InputRegister;
    profile.addressing = mdv::modbus::DirectSlaveAddressing{
        .logicalMin = 1U,
        .logicalMax = 1U,
        .registerOffset = 0U,
    };

    const auto plan = mdv::modbus::BuildScanPlan(profile);

    Require(plan[0].Supported(), "input-register probe disappeared");
    Require(
        plan[0].probe->space ==
            mdv::modbus::RegisterSpace::InputRegister,
        "probe data space was not preserved");
}

void TestInvalidProbeQuantityIsRejected()
{
    auto zero = BaseProfile();
    zero.probe.quantity = 0U;

    RequireScanPlanError(
        [&] {
            static_cast<void>(mdv::modbus::BuildScanPlan(zero));
        },
        "1..125");

    auto tooLarge = BaseProfile();
    tooLarge.probe.quantity = 126U;

    RequireScanPlanError(
        [&] {
            static_cast<void>(mdv::modbus::BuildScanPlan(tooLarge));
        },
        "1..125");
}

void TestProbeRangeOverflowIsRejected()
{
    auto profile = BaseProfile();
    profile.probe.read.address = 65535U;
    profile.probe.quantity = 2U;
    profile.addressing = mdv::modbus::DirectSlaveAddressing{
        .logicalMin = 1U,
        .logicalMax = 1U,
        .registerOffset = 0U,
    };

    RequireScanPlanError(
        [&] {
            static_cast<void>(mdv::modbus::BuildScanPlan(profile));
        },
        "overflows 16-bit");
}

void TestResolverOverflowIsReportedAsScanPlanError()
{
    auto profile = BaseProfile();
    profile.probe.read.address = 65530U;
    profile.probe.quantity = 1U;
    profile.addressing = mdv::modbus::DirectSlaveAddressing{
        .logicalMin = 1U,
        .logicalMax = 1U,
        .registerOffset = 10U,
    };

    RequireScanPlanError(
        [&] {
            static_cast<void>(mdv::modbus::BuildScanPlan(profile));
        },
        "logical address 1");
}

} // namespace

int main()
{
    try {
        TestDirectSlavePlanCoversEveryLogicalAddress();
        TestFixedStridePlanUsesResolvedOffsets();
        TestExplicitPlanPreservesUnsupportedCandidates();
        TestProbeDataSpaceIsPreserved();
        TestInvalidProbeQuantityIsRejected();
        TestProbeRangeOverflowIsRejected();
        TestResolverOverflowIsReportedAsScanPlanError();

        std::cout << "MDVWB Modbus scan planning tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus scan planning tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
