#include "modbus_resolver.h"

#include <iostream>
#include <optional>
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
void RequireResolverError(
    Function&& function,
    std::string_view expectedText)
{
    try {
        function();
    }
    catch (const mdv::modbus::ResolverError& error) {
        if (std::string_view(error.what()).find(expectedText) ==
            std::string_view::npos) {
            throw std::runtime_error(
                "resolver failed for the wrong reason: " +
                std::string(error.what()));
        }
        return;
    }

    throw std::runtime_error(
        "invalid resolver input was accepted: " +
        std::string(expectedText));
}

mdv::modbus::ModbusProfile BaseProfile()
{
    mdv::modbus::ModbusProfile profile;
    profile.id = "resolver_test";
    profile.name = "Resolver Test";
    profile.registerAddressing = "pdu_zero_based";
    return profile;
}

void TestGlobalLogicalBoundaries()
{
    auto profile = BaseProfile();
    profile.addressing = mdv::modbus::DirectSlaveAddressing{
        .logicalMin = 1,
        .logicalMax = 63,
        .registerOffset = 0,
    };

    RequireResolverError(
        [&] {
            static_cast<void>(
                mdv::modbus::ResolveLogicalAddress(profile, 0));
        },
        "1..63");

    RequireResolverError(
        [&] {
            static_cast<void>(
                mdv::modbus::ResolveLogicalAddress(profile, 64));
        },
        "1..63");
}

void TestDirectSlaveResolver()
{
    auto profile = BaseProfile();
    profile.addressing = mdv::modbus::DirectSlaveAddressing{
        .logicalMin = 1,
        .logicalMax = 63,
        .registerOffset = 5,
    };

    const auto first =
        mdv::modbus::ResolveLogicalAddress(profile, 1);
    Require(first.has_value(), "direct logical 1 was not resolved");
    Require(first->slaveId == 1, "direct logical 1 slave mismatch");
    Require(first->registerOffset == 5,
            "direct constant register offset mismatch");

    const auto last =
        mdv::modbus::ResolveLogicalAddress(profile, 63);
    Require(last.has_value(), "direct logical 63 was not resolved");
    Require(last->slaveId == 63, "direct logical 63 slave mismatch");

    auto limited = profile;
    limited.addressing = mdv::modbus::DirectSlaveAddressing{
        .logicalMin = 5,
        .logicalMax = 10,
        .registerOffset = 0,
    };
    Require(
        !mdv::modbus::ResolveLogicalAddress(limited, 4).has_value(),
        "valid but profile-unsupported direct candidate was resolved");
}

void TestFixedSlaveStrideResolver()
{
    auto profile = BaseProfile();
    profile.addressing = mdv::modbus::FixedSlaveStrideAddressing{
        .logicalMin = 1,
        .logicalMax = 63,
        .slaveId = 7,
        .firstLogicalAddress = 1,
        .registerStride = 91,
    };

    const auto one =
        mdv::modbus::ResolveLogicalAddress(profile, 1);
    Require(one.has_value(), "stride logical 1 was not resolved");
    Require(one->slaveId == 7, "stride slave mismatch");
    Require(one->registerOffset == 0,
            "stride logical 1 offset mismatch");

    const auto two =
        mdv::modbus::ResolveLogicalAddress(profile, 2);
    Require(two.has_value(), "stride logical 2 was not resolved");
    Require(two->registerOffset == 91,
            "stride logical 2 offset mismatch");

    const auto last =
        mdv::modbus::ResolveLogicalAddress(profile, 63);
    Require(last.has_value(), "stride logical 63 was not resolved");
    Require(last->registerOffset == 5642,
            "stride logical 63 offset mismatch");
}

void TestExplicitResolver()
{
    auto profile = BaseProfile();
    mdv::modbus::ExplicitAddressing explicitAddressing;
    explicitAddressing.logicalMin = 1;
    explicitAddressing.logicalMax = 5;
    explicitAddressing.devices.emplace(
        1,
        mdv::modbus::ExplicitDeviceLocation{
            .slaveId = 1,
            .registerOffset = 0,
        });
    explicitAddressing.devices.emplace(
        2,
        mdv::modbus::ExplicitDeviceLocation{
            .slaveId = 1,
            .registerOffset = 137,
        });
    explicitAddressing.devices.emplace(
        3,
        mdv::modbus::ExplicitDeviceLocation{
            .slaveId = 7,
            .registerOffset = 0,
        });
    profile.addressing = explicitAddressing;

    const auto second =
        mdv::modbus::ResolveLogicalAddress(profile, 2);
    Require(second.has_value(), "explicit logical 2 was not resolved");
    Require(second->slaveId == 1, "explicit slave mismatch");
    Require(second->registerOffset == 137,
            "explicit offset mismatch");

    const auto third =
        mdv::modbus::ResolveLogicalAddress(profile, 3);
    Require(third.has_value(), "explicit logical 3 was not resolved");
    Require(third->slaveId == 7, "explicit alternate slave mismatch");

    Require(
        !mdv::modbus::ResolveLogicalAddress(profile, 4).has_value(),
        "missing explicit mapping was not treated as unsupported");
    Require(
        !mdv::modbus::ResolveLogicalAddress(profile, 6).has_value(),
        "candidate outside explicit logical range was resolved");
}

void TestEffectiveRegisterLocation()
{
    auto profile = BaseProfile();
    profile.addressing = mdv::modbus::FixedSlaveStrideAddressing{
        .logicalMin = 1,
        .logicalMax = 63,
        .slaveId = 1,
        .firstLogicalAddress = 1,
        .registerStride = 91,
    };

    const mdv::modbus::RegisterLocation base{
        .space = mdv::modbus::RegisterSpace::HoldingRegister,
        .address = 27,
        .reference = "40028",
    };

    const auto resolved =
        mdv::modbus::ResolveRegisterLocation(profile, 2, base);
    Require(resolved.has_value(), "effective register was not resolved");
    Require(resolved->slaveId == 1, "effective register slave mismatch");
    Require(
        resolved->space ==
            mdv::modbus::RegisterSpace::HoldingRegister,
        "effective register space mismatch");
    Require(resolved->address == 118,
            "effective register address mismatch");
}

void TestEffectiveRegisterOverflow()
{
    auto profile = BaseProfile();
    profile.addressing = mdv::modbus::DirectSlaveAddressing{
        .logicalMin = 1,
        .logicalMax = 63,
        .registerOffset = 1000,
    };

    const mdv::modbus::RegisterLocation base{
        .space = mdv::modbus::RegisterSpace::HoldingRegister,
        .address = 65000,
        .reference = std::nullopt,
    };

    RequireResolverError(
        [&] {
            static_cast<void>(
                mdv::modbus::ResolveRegisterLocation(
                    profile, 1, base));
        },
        "overflows");
}

void TestMalformedFixedStrideProfileIsRejectedDefensively()
{
    auto profile = BaseProfile();
    profile.addressing = mdv::modbus::FixedSlaveStrideAddressing{
        .logicalMin = 1,
        .logicalMax = 10,
        .slaveId = 1,
        .firstLogicalAddress = 2,
        .registerStride = 91,
    };

    RequireResolverError(
        [&] {
            static_cast<void>(
                mdv::modbus::ResolveLogicalAddress(profile, 2));
        },
        "must equal logicalMin");
}

} // namespace

int main()
{
    try {
        TestGlobalLogicalBoundaries();
        TestDirectSlaveResolver();
        TestFixedSlaveStrideResolver();
        TestExplicitResolver();
        TestEffectiveRegisterLocation();
        TestEffectiveRegisterOverflow();
        TestMalformedFixedStrideProfileIsRejectedDefensively();

        std::cout << "MDVWB Modbus resolver tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus resolver tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
