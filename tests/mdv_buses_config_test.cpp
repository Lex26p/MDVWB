#include "mdv_buses_config.h"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void ExpectError(std::string_view json, std::string_view expectedPart) {
    try {
        static_cast<void>(mdvwb::ParseBusesConfig(json));
    } catch (const mdvwb::BusesConfigError& error) {
        Require(std::string(error.what()).find(expectedPart) != std::string::npos,
                "validation error does not contain expected text");
        return;
    }
    throw std::runtime_error("invalid configuration was accepted");
}

void TestValidConfigurationAndCanonicalOrder() {
    const auto config = mdvwb::ParseBusesConfig(R"json(
{
  "version": 1,
  "buses": [
    {"id": 3, "enabled": true, "port": "/dev/ttyUSB0", "addresses": [12, 2, 7]},
    {"id": 1, "enabled": true, "port": "/dev/ttyRS485-1", "addresses": [3, 1, 2]},
    {"id": 2, "enabled": false, "port": "/dev/serial/by-id/usb-MDV:2", "addresses": []}
  ]
}
)json");

    Require(config.version == 1, "wrong schema version");
    Require(config.buses.size() == 3, "wrong bus count");
    Require(config.buses[0].id == 1 && config.buses[1].id == 2 && config.buses[2].id == 3,
            "buses are not sorted by id");
    Require(config.buses[0].addresses == std::vector<int>({1, 2, 3}),
            "addresses are not sorted");
    Require(config.buses[1].addresses.empty(), "disabled bus must allow an empty address list");
}

void TestRoundTrip() {
    const std::string input = R"json({
      "version": 1,
      "buses": [
        {"id": 2, "enabled": true, "port": "/dev/ttyRS485-2", "addresses": [5, 1, 18]},
        {"id": 1, "enabled": true, "port": "/dev/ttyRS485-1", "addresses": [3, 2, 1]}
      ]
    })json";

    const auto first = mdvwb::ParseBusesConfig(input);
    const std::string serialized = mdvwb::SerializeBusesConfig(first);
    const auto second = mdvwb::ParseBusesConfig(serialized);

    Require(second.buses.size() == 2, "round trip changed bus count");
    Require(second.buses[0].id == 1, "round trip changed canonical bus order");
    Require(second.buses[1].addresses == std::vector<int>({1, 5, 18}),
            "round trip changed addresses");
}

void TestValidationErrors() {
    ExpectError(R"json({"version":1,"buses":[
      {"id":1,"enabled":true,"port":"/dev/a","addresses":[1]},
      {"id":1,"enabled":true,"port":"/dev/b","addresses":[2]}
    ]})json", "duplicate bus id");

    ExpectError(R"json({"version":1,"buses":[
      {"id":1,"enabled":true,"port":"/dev/a","addresses":[1]},
      {"id":2,"enabled":true,"port":"/dev/a","addresses":[2]}
    ]})json", "assigned to more than one bus");

    ExpectError(R"json({"version":1,"buses":[
      {"id":1,"enabled":true,"port":"/dev/a","addresses":[1,1]}
    ]})json", "duplicate address");

    ExpectError(R"json({"version":1,"buses":[
      {"id":1,"enabled":true,"port":"/dev/a","addresses":[64]}
    ]})json", "0..63");

    ExpectError(R"json({"version":1,"buses":[
      {"id":1,"enabled":true,"port":"ttyRS485-1","addresses":[1]}
    ]})json", "beginning with /dev/");

    ExpectError(R"json({"version":1,"buses":[
      {"id":1,"enabled":true,"port":"/dev/a","addresses":[]}
    ]})json", "enabled but has no polling addresses");

    ExpectError(R"json({"version":1,"buses":[
      {"id":1,"enabled":true,"port":"/dev/a","addresses":[1],"name":"extra"}
    ]})json", "unknown field");

    ExpectError(R"json({"version":2,"buses":[]})json", "must be in range 1..1");
    ExpectError(R"json({"version":1,"buses": [})json", "JSON error");
}

}  // namespace

int main() {
    try {
        TestValidConfigurationAndCanonicalOrder();
        TestRoundTrip();
        TestValidationErrors();
        std::cout << "MDVWB buses configuration tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB buses configuration tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
