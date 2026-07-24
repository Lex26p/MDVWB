#include "mdvwb_discovery_runner.h"

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void RequireThrows(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

}  // namespace

int main() {
    try {
        const auto addresses = mdvwb::ParseDiscoveryAddresses(
            "Discovery pass 1/3 started.\n"
            "Found MDV addresses: 1,3,18\n"
            "FOUND_ADDRESSES=1,3,18\n");
        Require(addresses == std::vector<int>({1, 3, 18}),
                "valid addresses were not parsed");

        const auto empty = mdvwb::ParseDiscoveryAddresses(
            "No MDV fan coils found.\nFOUND_ADDRESSES=\n");
        Require(empty.empty(), "empty discovery result was not parsed");

        const auto last = mdvwb::ParseDiscoveryAddresses(
            "FOUND_ADDRESSES=1\nFOUND_ADDRESSES=2,4\r\n");
        Require(last == std::vector<int>({2, 4}),
                "last machine-readable result was not used");

        RequireThrows(
            [] { static_cast<void>(mdvwb::ParseDiscoveryAddresses("no result\n")); },
            "missing result marker was accepted");
        RequireThrows(
            [] { static_cast<void>(mdvwb::ParseDiscoveryAddresses("FOUND_ADDRESSES=1,64\n")); },
            "out-of-range address was accepted");
        RequireThrows(
            [] { static_cast<void>(mdvwb::ParseDiscoveryAddresses("FOUND_ADDRESSES=1,1\n")); },
            "duplicate address was accepted");
        RequireThrows(
            [] { static_cast<void>(mdvwb::ParseDiscoveryAddresses("FOUND_ADDRESSES=1,,2\n")); },
            "empty address was accepted");

        std::cout << "MDVWB discovery runner tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB discovery runner tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
