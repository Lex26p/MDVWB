#include "mdv_modbus_bus_config.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

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
void ExpectConfigError(
    Function&& function,
    std::string_view expectedPart)
{
    try {
        function();
    }
    catch (const mdvwb::BusesConfigError& error) {
        Require(
            std::string_view(error.what()).find(expectedPart) !=
                std::string_view::npos,
            "configuration error does not contain expected text");
        return;
    }

    throw std::runtime_error("invalid Modbus bus/profile binding was accepted");
}

mdv::modbus::ProfileCatalog LoadProductionCatalog()
{
    return mdv::modbus::LoadProfileDirectory(
        std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus");
}

mdvwb::BusesConfig ParseBus(
    std::string_view profileId = "vrf_add_controller",
    int baudRate = 9600,
    int dataBits = 8,
    std::string_view parity = "none",
    int stopBits = 1,
    std::string_view addresses = "[1,2,63]")
{
    const std::string json =
        "{\"version\":1,\"buses\":[{"
        "\"id\":2,"
        "\"enabled\":true,"
        "\"protocol\":\"modbus_rtu\","
        "\"port\":\"/dev/ttyRS485-2\","
        "\"modbus\":{"
        "\"profileId\":\"" + std::string(profileId) + "\","
        "\"baudRate\":" + std::to_string(baudRate) + ","
        "\"dataBits\":" + std::to_string(dataBits) + ","
        "\"parity\":\"" + std::string(parity) + "\","
        "\"stopBits\":" + std::to_string(stopBits) + "},"
        "\"addresses\":" + std::string(addresses) +
        "}]}";
    return mdvwb::ParseBusesConfig(json);
}

mdv::modbus::ModbusProfile NarrowProfile()
{
    return mdv::modbus::ParseProfile(R"json(
{
  "schemaVersion": 1,
  "id": "narrow_profile",
  "name": "Narrow profile",
  "registerAddressing": "pdu_zero_based",
  "transport": {
    "baudRate": 9600,
    "dataBits": 8,
    "parity": "none",
    "stopBits": 1
  },
  "addressing": {
    "type": "direct_slave",
    "logicalMin": 1,
    "logicalMax": 2
  },
  "capabilities": {
    "power": true
  },
  "probe": {
    "read": {
      "space": "holding_register",
      "address": 100
    },
    "quantity": 1
  },
  "points": {
    "power": {
      "type": "boolean",
      "read": {
        "space": "holding_register",
        "address": 100
      }
    }
  }
}
)json");
}

void TestProductionProfileResolves()
{
    const auto catalog = LoadProductionCatalog();
    Require(!catalog.HasErrors(), "production profile catalog has load errors");

    const auto config = ParseBus();
    mdvwb::ValidateModbusBusProfiles(config, catalog);

    const auto& profile = mdvwb::ResolveModbusBusProfile(
        config.buses.front(),
        catalog);
    Require(
        profile.id == "vrf_add_controller",
        "resolved the wrong production Modbus profile");
}

void TestUnknownProfileRejected()
{
    const auto catalog = LoadProductionCatalog();
    const auto config = ParseBus("missing_profile");

    ExpectConfigError(
        [&] {
            mdvwb::ValidateModbusBusProfiles(config, catalog);
        },
        "unknown Modbus profile 'missing_profile'");
}

void TestTransportMismatchRejected()
{
    const auto catalog = LoadProductionCatalog();

    ExpectConfigError(
        [&] {
            const auto config = ParseBus("vrf_add_controller", 19200);
            mdvwb::ValidateModbusBusProfiles(config, catalog);
        },
        "baudRate 19200 does not match");

    ExpectConfigError(
        [&] {
            const auto config = ParseBus(
                "vrf_add_controller", 9600, 7);
            mdvwb::ValidateModbusBusProfiles(config, catalog);
        },
        "dataBits 7 does not match");

    ExpectConfigError(
        [&] {
            const auto config = ParseBus(
                "vrf_add_controller", 9600, 8, "even");
            mdvwb::ValidateModbusBusProfiles(config, catalog);
        },
        "parity 'even' does not match");

    ExpectConfigError(
        [&] {
            const auto config = ParseBus(
                "vrf_add_controller", 9600, 8, "none", 2);
            mdvwb::ValidateModbusBusProfiles(config, catalog);
        },
        "stopBits 2 does not match");
}

void TestConfiguredAddressMustBeSupportedByProfile()
{
    mdv::modbus::ProfileCatalog catalog;
    catalog.profiles.emplace("narrow_profile", NarrowProfile());

    const auto config = ParseBus(
        "narrow_profile",
        9600,
        8,
        "none",
        1,
        "[1,3]");

    ExpectConfigError(
        [&] {
            mdvwb::ValidateModbusBusProfiles(config, catalog);
        },
        "logical address 3 is not supported");
}

void TestMdvBusDoesNotRequireProfileCatalog()
{
    const auto config = mdvwb::ParseBusesConfig(R"json(
{
  "version": 1,
  "buses": [
    {
      "id": 1,
      "enabled": true,
      "port": "/dev/ttyRS485-1",
      "addresses": [0,1,63]
    }
  ]
}
)json");

    const mdv::modbus::ProfileCatalog emptyCatalog;
    mdvwb::ValidateModbusBusProfiles(config, emptyCatalog);
}

void TestUnrelatedCatalogIssueDoesNotInvalidateSelectedProfile()
{
    auto catalog = LoadProductionCatalog();
    catalog.issues.push_back({
        .path = "broken-unrelated.json",
        .error = "synthetic unrelated profile error",
    });

    const auto config = ParseBus();
    mdvwb::ValidateModbusBusProfiles(config, catalog);
}

void TestResolverRejectsMdvBus()
{
    const auto config = mdvwb::ParseBusesConfig(R"json(
{
  "version": 1,
  "buses": [
    {
      "id": 1,
      "enabled": true,
      "protocol": "mdv",
      "port": "/dev/ttyRS485-1",
      "addresses": [1]
    }
  ]
}
)json");

    const auto catalog = LoadProductionCatalog();
    ExpectConfigError(
        [&] {
            static_cast<void>(mdvwb::ResolveModbusBusProfile(
                config.buses.front(),
                catalog));
        },
        "requires protocol modbus_rtu");
}

} // namespace

int main()
{
    try {
        TestProductionProfileResolves();
        TestUnknownProfileRejected();
        TestTransportMismatchRejected();
        TestConfiguredAddressMustBeSupportedByProfile();
        TestMdvBusDoesNotRequireProfileCatalog();
        TestUnrelatedCatalogIssueDoesNotInvalidateSelectedProfile();
        TestResolverRejectsMdvBus();

        std::cout << "MDVWB Modbus bus profile validation tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr
            << "MDVWB Modbus bus profile validation tests: FAILED: "
            << error.what() << '\n';
        return 1;
    }
}
