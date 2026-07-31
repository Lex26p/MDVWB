#include "mdv_buses_config.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void ExpectError(
    std::string_view json,
    std::string_view expectedPart)
{
    try {
        static_cast<void>(
            mdvwb::ParseBusesConfig(json));
    }
    catch (const mdvwb::BusesConfigError& error) {
        Require(
            std::string(error.what()).find(expectedPart) !=
                std::string::npos,
            "validation error does not contain expected text");
        return;
    }

    throw std::runtime_error(
        "invalid configuration was accepted");
}

void TestLegacyBusDefaultsToMdv()
{
    const auto config = mdvwb::ParseBusesConfig(R"json(
{
  "version": 1,
  "revision": 4,
  "buses": [
    {
      "id": 1,
      "enabled": true,
      "port": "/dev/ttyRS485-1",
      "addresses": [0, 1, 63]
    }
  ]
}
)json");

    Require(config.buses.size() == 1U, "legacy bus count mismatch");
    const auto& bus = config.buses.front();

    Require(
        bus.protocol == mdvwb::BusProtocol::Mdv,
        "legacy bus without protocol did not remain MDV");
    Require(
        !bus.modbus.has_value(),
        "legacy MDV bus gained Modbus settings");
    Require(
        bus.addresses == std::vector<int>({0, 1, 63}),
        "legacy MDV addresses changed");

    const auto serialized =
        mdvwb::SerializeBusesConfig(config);
    Require(
        serialized.find("\"protocol\": \"mdv\"") !=
            std::string::npos,
        "canonical serializer did not make MDV protocol explicit");
}

void TestExplicitMdvBus()
{
    const auto config = mdvwb::ParseBusesConfig(R"json(
{
  "version": 1,
  "buses": [
    {
      "id": 2,
      "enabled": true,
      "protocol": "mdv",
      "port": "/dev/ttyUSB0",
      "addresses": [5, 0]
    }
  ]
}
)json");

    Require(
        config.buses.front().protocol ==
            mdvwb::BusProtocol::Mdv,
        "explicit MDV protocol mismatch");
    Require(
        config.buses.front().addresses ==
            std::vector<int>({0, 5}),
        "explicit MDV addresses were not normalized");
}

void TestValidModbusBus()
{
    const auto config = mdvwb::ParseBusesConfig(R"json(
{
  "version": 1,
  "revision": 9,
  "buses": [
    {
      "id": 7,
      "enabled": true,
      "protocol": "modbus_rtu",
      "port": "/dev/ttyRS485-2",
      "modbus": {
        "profileId": "vrf_add_controller",
        "baudRate": 9600,
        "dataBits": 8,
        "parity": "none",
        "stopBits": 1
      },
      "addresses": [63, 1, 2]
    }
  ]
}
)json");

    Require(config.buses.size() == 1U, "Modbus bus count mismatch");

    const auto& bus = config.buses.front();
    Require(
        bus.protocol == mdvwb::BusProtocol::ModbusRtu,
        "Modbus protocol mismatch");
    Require(bus.modbus.has_value(), "Modbus settings missing");
    Require(
        bus.modbus->profileId == "vrf_add_controller",
        "Modbus profile id mismatch");
    Require(bus.modbus->baudRate == 9600, "Modbus baud mismatch");
    Require(bus.modbus->dataBits == 8, "Modbus data bits mismatch");
    Require(
        bus.modbus->parity == mdvwb::BusParity::None,
        "Modbus parity mismatch");
    Require(bus.modbus->stopBits == 1, "Modbus stop bits mismatch");
    Require(
        bus.addresses == std::vector<int>({1, 2, 63}),
        "Modbus addresses were not normalized");

    const auto serialized =
        mdvwb::SerializeBusesConfig(config);
    const auto roundTrip =
        mdvwb::ParseBusesConfig(serialized);
    Require(
        roundTrip.buses.front().protocol ==
            mdvwb::BusProtocol::ModbusRtu,
        "round trip changed Modbus protocol");
    Require(
        roundTrip.buses.front().modbus->profileId ==
            "vrf_add_controller",
        "round trip changed profile id");
}

void TestProtocolSpecificValidation()
{
    ExpectError(R"json({
      "version": 1,
      "buses": [{
        "id": 1,
        "enabled": true,
        "protocol": "modbus_rtu",
        "port": "/dev/a",
        "addresses": [1]
      }]
    })json", "missing required field 'modbus'");

    ExpectError(R"json({
      "version": 1,
      "buses": [{
        "id": 1,
        "enabled": true,
        "protocol": "mdv",
        "port": "/dev/a",
        "modbus": {
          "profileId": "vrf_add_controller",
          "baudRate": 9600,
          "dataBits": 8,
          "parity": "none",
          "stopBits": 1
        },
        "addresses": [1]
      }]
    })json", "allowed only when protocol is modbus_rtu");

    ExpectError(R"json({
      "version": 1,
      "buses": [{
        "id": 1,
        "enabled": true,
        "protocol": "modbus_rtu",
        "port": "/dev/a",
        "modbus": {
          "profileId": "vrf_add_controller",
          "baudRate": 9600,
          "dataBits": 8,
          "parity": "none",
          "stopBits": 1
        },
        "addresses": [0]
      }]
    })json", "1..63 for protocol modbus_rtu");

    ExpectError(R"json({
      "version": 1,
      "buses": [{
        "id": 1,
        "enabled": true,
        "protocol": "modbus_rtu",
        "port": "/dev/a",
        "modbus": {
          "profileId": "Bad-Profile",
          "baudRate": 9600,
          "dataBits": 8,
          "parity": "none",
          "stopBits": 1
        },
        "addresses": [1]
      }]
    })json", "profileId must start with a-z");

    ExpectError(R"json({
      "version": 1,
      "buses": [{
        "id": 1,
        "enabled": true,
        "protocol": "modbus_rtu",
        "port": "/dev/a",
        "modbus": {
          "profileId": "vrf_add_controller",
          "baudRate": 12345,
          "dataBits": 8,
          "parity": "none",
          "stopBits": 1
        },
        "addresses": [1]
      }]
    })json", "baudRate must be one of");

    ExpectError(R"json({
      "version": 1,
      "buses": [{
        "id": 1,
        "enabled": true,
        "protocol": "modbus_rtu",
        "port": "/dev/a",
        "modbus": {
          "profileId": "vrf_add_controller",
          "baudRate": 9600,
          "dataBits": 6,
          "parity": "none",
          "stopBits": 1
        },
        "addresses": [1]
      }]
    })json", "dataBits must be 7 or 8");

    ExpectError(R"json({
      "version": 1,
      "buses": [{
        "id": 1,
        "enabled": true,
        "protocol": "modbus_rtu",
        "port": "/dev/a",
        "modbus": {
          "profileId": "vrf_add_controller",
          "baudRate": 9600,
          "dataBits": 8,
          "parity": "mark",
          "stopBits": 1
        },
        "addresses": [1]
      }]
    })json", "parity must be one of none, even, odd");

    ExpectError(R"json({
      "version": 1,
      "buses": [{
        "id": 1,
        "enabled": true,
        "protocol": "other",
        "port": "/dev/a",
        "addresses": [1]
      }]
    })json", "must be one of mdv, modbus_rtu");
}

}  // namespace

int main()
{
    try {
        TestLegacyBusDefaultsToMdv();
        TestExplicitMdvBus();
        TestValidModbusBus();
        TestProtocolSpecificValidation();

        std::cout
            << "MDVWB Modbus buses configuration tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr
            << "MDVWB Modbus buses configuration tests: FAILED: "
            << error.what() << '\n';
        return 1;
    }
}
