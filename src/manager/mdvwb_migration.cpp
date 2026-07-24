#include "mdvwb_migration.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mdvwb {
namespace {

std::string Trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

std::string Unquote(std::string value) {
    value = Trim(std::move(value));
    if (value.size() >= 2U &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2U);
    }
    return value;
}

std::map<std::string, std::string> ReadAssignments(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read legacy configuration: " + path.string());
    }

    std::map<std::string, std::string> values;
    std::string line;
    while (std::getline(input, line)) {
        line = Trim(std::move(line));
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, separator));
        if (key.rfind("MDVWB_", 0) != 0U) {
            continue;
        }
        values[key] = Unquote(line.substr(separator + 1U));
    }
    return values;
}

int ParseInteger(std::string_view text, std::string_view field) {
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw BusesConfigError(
            "legacy " + std::string(field) + " must be an integer");
    }
    return value;
}

std::vector<int> ParseAddresses(std::string_view text) {
    std::vector<int> addresses;
    std::set<int> unique;
    while (!text.empty()) {
        const std::size_t separator = text.find(',');
        std::string token = Trim(std::string(text.substr(0, separator)));
        if (token.empty()) {
            throw BusesConfigError("legacy address list contains an empty item");
        }
        const int address = ParseInteger(token, "address");
        if (address < 0 || address > 63) {
            throw BusesConfigError("legacy address must be in range 0..63");
        }
        if (!unique.insert(address).second) {
            throw BusesConfigError("legacy address list contains duplicates");
        }
        addresses.push_back(address);
        if (separator == std::string_view::npos) {
            break;
        }
        text.remove_prefix(separator + 1U);
    }
    std::sort(addresses.begin(), addresses.end());
    return addresses;
}

std::optional<int> BusIdFromFilename(std::string_view filename) {
    if (filename == "mdvwb") {
        return std::nullopt;
    }
    constexpr std::string_view prefix = "mdvwb-";
    if (filename.rfind(prefix, 0) != 0U) {
        return std::nullopt;
    }
    const std::string_view idText = filename.substr(prefix.size());
    if (idText.empty()) {
        return std::nullopt;
    }
    const int id = ParseInteger(idText, "bus id");
    return id;
}

} // namespace

BusesConfig MigrateLegacyDefaults(
    const ServiceSyncPaths& paths,
    CommandRunner& commandRunner) {
    std::map<int, BusConfig> buses;
    std::error_code error;
    if (!std::filesystem::is_directory(paths.defaultDirectory, error)) {
        throw std::runtime_error(
            "legacy configuration directory does not exist: " +
            paths.defaultDirectory.string());
    }

    for (const auto& entry : std::filesystem::directory_iterator(paths.defaultDirectory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        if (filename != "mdvwb" && filename.rfind("mdvwb-", 0) != 0U) {
            continue;
        }

        const auto values = ReadAssignments(entry.path());
        const auto busValue = values.find("MDVWB_BUS");
        const auto portValue = values.find("MDVWB_PORT");
        const auto addressesValue = values.find("MDVWB_ADDRESSES");
        if (portValue == values.end() || addressesValue == values.end()) {
            continue;
        }

        int busId = 0;
        if (busValue != values.end() && !busValue->second.empty()) {
            busId = ParseInteger(busValue->second, "bus id");
        } else {
            const auto filenameId = BusIdFromFilename(filename);
            if (!filenameId.has_value()) {
                continue;
            }
            busId = *filenameId;
        }
        if (busId < 1 || busId > 999) {
            throw BusesConfigError("legacy bus id must be in range 1..999");
        }

        BusConfig bus;
        bus.id = busId;
        bus.port = portValue->second;
        bus.addresses = ParseAddresses(addressesValue->second);
        const BusServiceStatus status =
            QueryBusServiceStatus(busId, paths, commandRunner);
        bus.enabled = status.active || status.enabled;

        const auto iterator = buses.find(busId);
        if (iterator == buses.end()) {
            buses.emplace(busId, std::move(bus));
        } else if (filename != "mdvwb") {
            // Prefer the explicit mdvwb-N file over the old unsuffixed file.
            iterator->second = std::move(bus);
        }
    }

    if (buses.empty()) {
        throw std::runtime_error("no legacy MDVWB bus configurations were found");
    }

    BusesConfig result;
    for (auto& [id, bus] : buses) {
        static_cast<void>(id);
        result.buses.push_back(std::move(bus));
    }
    // Re-parse the canonical JSON so all normal validation rules are applied.
    return ParseBusesConfig(SerializeBusesConfig(result));
}

} // namespace mdvwb
