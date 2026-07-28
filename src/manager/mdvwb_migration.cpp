#include "mdvwb_migration.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mdvwb {
namespace {

struct LegacyAssignment {
    std::string value;
    std::size_t line = 0;
};

struct LegacyBusSource {
    BusConfig bus;
    std::filesystem::path path;
};

std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

[[noreturn]] void FailLegacy(
    const std::filesystem::path& path,
    std::optional<std::size_t> line,
    std::string message)
{
    std::string detail = "legacy configuration '" + path.string() + "'";
    if (line.has_value()) {
        detail += " line " + std::to_string(*line);
    }
    throw BusesConfigError(detail + ": " + std::move(message));
}

bool IsAssignmentKey(std::string_view key)
{
    if (key.empty()) {
        return false;
    }
    const auto isAlphaOrUnderscore = [](char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        return std::isalpha(byte) != 0 || character == '_';
    };
    const auto isAlphaNumericOrUnderscore = [](char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '_';
    };
    return isAlphaOrUnderscore(key.front()) &&
        std::all_of(
            key.begin() + 1,
            key.end(),
            isAlphaNumericOrUnderscore);
}

std::string ParseAssignmentValue(
    std::string value,
    const std::filesystem::path& path,
    std::size_t line,
    std::string_view key)
{
    value = Trim(std::move(value));
    if (value.empty()) {
        return {};
    }

    const char first = value.front();
    if (first != '\'' && first != '"') {
        return value;
    }
    if (value.size() < 2U || value.back() != first) {
        FailLegacy(
            path,
            line,
            "unterminated quoted value for " + std::string(key));
    }

    for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
        if (value[index] == first && value[index - 1U] != '\\') {
            FailLegacy(
                path,
                line,
                "unexpected quote inside value for " + std::string(key));
        }
    }
    return value.substr(1U, value.size() - 2U);
}

std::map<std::string, LegacyAssignment> ReadAssignments(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot read legacy configuration: " + path.string());
    }

    std::map<std::string, LegacyAssignment> values;
    std::string lineText;
    std::size_t lineNumber = 0;
    while (std::getline(input, lineText)) {
        ++lineNumber;
        lineText = Trim(std::move(lineText));
        if (lineText.empty() || lineText.front() == '#') {
            continue;
        }

        const std::size_t separator = lineText.find('=');
        if (separator == std::string::npos) {
            FailLegacy(path, lineNumber, "expected NAME=VALUE assignment");
        }
        const std::string key = Trim(lineText.substr(0U, separator));
        if (!IsAssignmentKey(key)) {
            FailLegacy(path, lineNumber, "invalid assignment name '" + key + "'");
        }
        if (key.rfind("MDVWB_", 0U) != 0U) {
            continue;
        }
        if (values.contains(key)) {
            FailLegacy(
                path,
                lineNumber,
                "duplicate assignment for " + key +
                    " (first declared at line " +
                    std::to_string(values.at(key).line) + ")");
        }

        values.emplace(
            key,
            LegacyAssignment{
                ParseAssignmentValue(
                    lineText.substr(separator + 1U),
                    path,
                    lineNumber,
                    key),
                lineNumber});
    }
    if (!input.good() && !input.eof()) {
        throw std::runtime_error(
            "cannot finish reading legacy configuration: " + path.string());
    }
    return values;
}

int ParseInteger(
    std::string_view text,
    std::string_view field,
    const std::filesystem::path& path,
    std::optional<std::size_t> line = std::nullopt)
{
    int value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) {
        FailLegacy(path, line, std::string(field) + " must be an integer");
    }
    return value;
}

std::vector<int> ParseAddresses(
    std::string_view text,
    const std::filesystem::path& path,
    std::size_t line)
{
    std::vector<int> addresses;
    std::set<int> unique;
    if (text.empty()) {
        return addresses;
    }

    while (!text.empty()) {
        const std::size_t separator = text.find(',');
        const std::string token =
            Trim(std::string(text.substr(0U, separator)));
        if (token.empty()) {
            FailLegacy(path, line, "address list contains an empty item");
        }
        const int address = ParseInteger(token, "address", path, line);
        if (address < 0 || address > 63) {
            FailLegacy(path, line, "address must be in range 0..63");
        }
        if (!unique.insert(address).second) {
            FailLegacy(path, line, "address list contains duplicates");
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

bool IsLegacyCandidateName(std::string_view filename)
{
    if (filename == "mdvwb") {
        return true;
    }
    constexpr std::string_view prefix = "mdvwb-";
    if (!filename.starts_with(prefix)) {
        return false;
    }
    const std::string_view suffix = filename.substr(prefix.size());
    if (suffix.empty()) {
        return true;
    }
    return std::all_of(suffix.begin(), suffix.end(), [](char character) {
        return std::isdigit(static_cast<unsigned char>(character)) != 0;
    });
}

std::optional<int> BusIdFromFilename(
    const std::filesystem::path& path)
{
    const std::string filename = path.filename().string();
    if (filename == "mdvwb") {
        return std::nullopt;
    }

    constexpr std::string_view prefix = "mdvwb-";
    const std::string_view idText(filename.data() + prefix.size(),
                                  filename.size() - prefix.size());
    if (idText.empty()) {
        FailLegacy(path, std::nullopt, "filename is missing a bus id");
    }
    if (idText.size() > 1U && idText.front() == '0') {
        FailLegacy(
            path,
            std::nullopt,
            "filename bus id must use canonical decimal form without leading zeros");
    }
    const int id = ParseInteger(idText, "filename bus id", path);
    if (id < 1 || id > 999) {
        FailLegacy(path, std::nullopt, "filename bus id must be in range 1..999");
    }
    return id;
}

const LegacyAssignment& RequireAssignment(
    const std::map<std::string, LegacyAssignment>& values,
    std::string_view key,
    const std::filesystem::path& path)
{
    const auto iterator = values.find(std::string(key));
    if (iterator == values.end()) {
        FailLegacy(
            path,
            std::nullopt,
            "missing required assignment " + std::string(key));
    }
    return iterator->second;
}

LegacyBusSource ParseLegacyBus(const std::filesystem::path& path)
{
    const auto filenameBusId = BusIdFromFilename(path);
    const auto values = ReadAssignments(path);
    const LegacyAssignment& port =
        RequireAssignment(values, "MDVWB_PORT", path);
    const LegacyAssignment& addresses =
        RequireAssignment(values, "MDVWB_ADDRESSES", path);

    std::optional<int> declaredBusId;
    if (const auto iterator = values.find("MDVWB_BUS");
        iterator != values.end()) {
        if (iterator->second.value.empty()) {
            FailLegacy(
                path,
                iterator->second.line,
                "MDVWB_BUS must not be empty");
        }
        const int parsed = ParseInteger(
            iterator->second.value,
            "bus id",
            path,
            iterator->second.line);
        if (parsed < 1 || parsed > 999) {
            FailLegacy(
                path,
                iterator->second.line,
                "bus id must be in range 1..999");
        }
        declaredBusId = parsed;
    }

    int busId = 0;
    if (filenameBusId.has_value()) {
        busId = *filenameBusId;
        if (declaredBusId.has_value() && *declaredBusId != busId) {
            FailLegacy(
                path,
                values.at("MDVWB_BUS").line,
                "ambiguous bus id: filename selects " +
                    std::to_string(busId) + " but MDVWB_BUS selects " +
                    std::to_string(*declaredBusId));
        }
    } else {
        if (!declaredBusId.has_value()) {
            FailLegacy(
                path,
                std::nullopt,
                "unsuffixed mdvwb file requires MDVWB_BUS");
        }
        busId = *declaredBusId;
    }

    BusConfig bus;
    bus.id = busId;
    bus.port = port.value;
    bus.addresses = ParseAddresses(addresses.value, path, addresses.line);
    return LegacyBusSource{std::move(bus), path};
}

}  // namespace

BusesConfig MigrateLegacyDefaults(
    const ServiceSyncPaths& paths,
    CommandRunner& commandRunner)
{
    std::error_code error;
    if (!std::filesystem::is_directory(paths.defaultDirectory, error)) {
        const std::string suffix = error ? ": " + error.message() : std::string{};
        throw std::runtime_error(
            "legacy configuration directory does not exist: " +
            paths.defaultDirectory.string() + suffix);
    }

    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator iterator(
             paths.defaultDirectory,
             error),
         end;
         iterator != end;
         iterator.increment(error)) {
        if (error) {
            throw std::runtime_error(
                "cannot enumerate legacy configuration directory: " +
                error.message());
        }
        const std::string filename = iterator->path().filename().string();
        if (!IsLegacyCandidateName(filename)) {
            continue;
        }
        std::error_code typeError;
        if (!iterator->is_regular_file(typeError)) {
            const std::string suffix = typeError
                ? ": " + typeError.message()
                : std::string{};
            FailLegacy(
                iterator->path(),
                std::nullopt,
                "candidate is not a regular file" + suffix);
        }
        candidates.push_back(iterator->path());
    }
    if (error) {
        throw std::runtime_error(
            "cannot enumerate legacy configuration directory: " +
            error.message());
    }

    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const auto& left, const auto& right) {
            return left.filename().string() < right.filename().string();
        });
    if (candidates.empty()) {
        throw std::runtime_error(
            "no legacy MDVWB bus configurations were found");
    }

    std::map<int, LegacyBusSource> buses;
    for (const std::filesystem::path& candidatePath : candidates) {
        LegacyBusSource source = ParseLegacyBus(candidatePath);
        const int busId = source.bus.id;
        const auto existing = buses.find(busId);
        if (existing != buses.end()) {
            throw BusesConfigError(
                "ambiguous legacy bus " + std::to_string(busId) +
                ": defined by both '" + existing->second.path.string() +
                "' and '" + source.path.string() + "'");
        }
        buses.emplace(busId, std::move(source));
    }

    BusesConfig result;
    for (auto& [busId, source] : buses) {
        static_cast<void>(busId);
        result.buses.push_back(std::move(source.bus));
    }

    // Validate ids, ports, port collisions and addresses before querying systemd.
    // Every bus is temporarily disabled, so an intentionally empty address list
    // can still be migrated when the corresponding service is disabled.
    result = ParseBusesConfig(SerializeBusesConfig(result));

    for (BusConfig& bus : result.buses) {
        const BusServiceStatus status =
            QueryBusServiceStatus(bus.id, paths, commandRunner);
        bus.enabled = status.active || status.enabled;
    }

    // Validate the final enabled state as well: an enabled bus must have at least
    // one polling address.
    return ParseBusesConfig(SerializeBusesConfig(result));
}

}  // namespace mdvwb
