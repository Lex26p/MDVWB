#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mdvwb {

struct BusConfig {
    int id = 0;
    bool enabled = false;
    std::string port;
    std::vector<int> addresses;
};

struct BusesConfig {
    int version = 1;
    std::vector<BusConfig> buses;
};

class BusesConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

BusesConfig ParseBusesConfig(std::string_view jsonText);
BusesConfig LoadBusesConfig(const std::filesystem::path& path);
std::string SerializeBusesConfig(const BusesConfig& config);

}  // namespace mdvwb
