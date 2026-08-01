#include "mdv_modbus_discovery.h"

#include "modbus_profile.h"
#include "modbus_scan_execute.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mdvwb {
namespace {

constexpr std::string_view ManagedMarker =
    "# Managed by mdvwb-manager from buses.json.";

[[nodiscard]] std::string Trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1U);
}

[[nodiscard]] std::string ParseAssignmentValue(
    std::string value,
    const std::filesystem::path& path,
    std::size_t line)
{
    value = Trim(std::move(value));
    if (value.empty()) {
        return {};
    }
    if (value.front() != '"' && value.front() != '\'') {
        return value;
    }

    const char quote = value.front();
    if (value.size() < 2U || value.back() != quote) {
        throw std::runtime_error(
            "managed discovery configuration has an unterminated value at line " +
            std::to_string(line) + " in " + path.filename().string());
    }

    std::string result;
    result.reserve(value.size() - 2U);
    for (std::size_t index = 1U; index + 1U < value.size(); ++index) {
        const char character = value[index];
        if (quote == '"' && character == '\\') {
            if (index + 2U >= value.size()) {
                throw std::runtime_error(
                    "managed discovery configuration has an invalid escape at line " +
                    std::to_string(line) + " in " + path.filename().string());
            }
            const char escaped = value[++index];
            if (escaped != '\\' && escaped != '"' &&
                escaped != '$' && escaped != '`') {
                throw std::runtime_error(
                    "managed discovery configuration has an unsupported escape at line " +
                    std::to_string(line) + " in " + path.filename().string());
            }
            result.push_back(escaped);
            continue;
        }
        if (character == quote) {
            throw std::runtime_error(
                "managed discovery configuration has an unexpected quote at line " +
                std::to_string(line) + " in " + path.filename().string());
        }
        result.push_back(character);
    }
    return result;
}

[[nodiscard]] std::map<std::string, std::string> ReadManagedAssignments(
    const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot read managed discovery configuration " +
            path.filename().string());
    }

    std::string firstLine;
    if (!std::getline(input, firstLine)) {
        return {};
    }
    if (Trim(firstLine) != ManagedMarker) {
        return {};
    }

    std::map<std::string, std::string> values;
    std::string lineText;
    std::size_t lineNumber = 1U;
    while (std::getline(input, lineText)) {
        ++lineNumber;
        lineText = Trim(std::move(lineText));
        if (lineText.empty() || lineText.front() == '#') {
            continue;
        }
        const std::size_t separator = lineText.find('=');
        if (separator == std::string::npos) {
            throw std::runtime_error(
                "managed discovery configuration expects NAME=VALUE at line " +
                std::to_string(lineNumber) + " in " + path.filename().string());
        }
        const std::string key = Trim(lineText.substr(0U, separator));
        if (!key.starts_with("MDVWB_")) {
            continue;
        }
        if (values.contains(key)) {
            throw std::runtime_error(
                "managed discovery configuration repeats " + key +
                " in " + path.filename().string());
        }
        values.emplace(
            key,
            ParseAssignmentValue(
                lineText.substr(separator + 1U),
                path,
                lineNumber));
    }
    if (!input.good() && !input.eof()) {
        throw std::runtime_error(
            "cannot finish reading managed discovery configuration " +
            path.filename().string());
    }
    return values;
}

[[nodiscard]] int ParseInteger(
    const std::map<std::string, std::string>& values,
    std::string_view key,
    int fallback,
    bool required = false)
{
    const auto iterator = values.find(std::string(key));
    if (iterator == values.end() || iterator->second.empty()) {
        if (required) {
            throw std::runtime_error(
                "managed Modbus discovery configuration is missing " +
                std::string(key));
        }
        return fallback;
    }

    int value = 0;
    const std::string& text = iterator->second;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) {
        throw std::runtime_error(
            "managed Modbus discovery configuration has an invalid " +
            std::string(key));
    }
    return value;
}

[[nodiscard]] std::string RequireValue(
    const std::map<std::string, std::string>& values,
    std::string_view key)
{
    const auto iterator = values.find(std::string(key));
    if (iterator == values.end() || iterator->second.empty()) {
        throw std::runtime_error(
            "managed Modbus discovery configuration is missing " +
            std::string(key));
    }
    return iterator->second;
}

[[nodiscard]] mdv::SerialParity ParseParity(std::string_view value)
{
    if (value == "none") {
        return mdv::SerialParity::None;
    }
    if (value == "even") {
        return mdv::SerialParity::Even;
    }
    if (value == "odd") {
        return mdv::SerialParity::Odd;
    }
    throw std::runtime_error(
        "managed Modbus discovery configuration has an invalid parity");
}

[[nodiscard]] DiscoveryRuntimeSelection SelectionFromAssignments(
    const std::map<std::string, std::string>& values,
    std::string port)
{
    const auto protocolIterator = values.find("MDVWB_PROTOCOL");
    const std::string protocol =
        protocolIterator == values.end() || protocolIterator->second.empty()
            ? "mdv"
            : protocolIterator->second;

    if (protocol == "mdv") {
        return DiscoveryRuntimeSelection{
            .protocol = DiscoveryRuntimeProtocol::Mdv,
            .modbus = std::nullopt,
        };
    }
    if (protocol != "modbus_rtu") {
        throw std::runtime_error(
            "managed discovery configuration uses an unsupported protocol");
    }

    ModbusDiscoveryRuntime runtime;
    runtime.port = std::move(port);
    runtime.profileDirectory = RequireValue(
        values,
        "MDVWB_MODBUS_PROFILE_DIR");
    runtime.profileId = RequireValue(values, "MDVWB_MODBUS_PROFILE");
    runtime.serial = mdv::SerialSettings{
        .baudRate = static_cast<std::uint32_t>(ParseInteger(
            values, "MDVWB_MODBUS_BAUD_RATE", 9600, true)),
        .dataBits = static_cast<std::uint8_t>(ParseInteger(
            values, "MDVWB_MODBUS_DATA_BITS", 8, true)),
        .parity = ParseParity(RequireValue(values, "MDVWB_MODBUS_PARITY")),
        .stopBits = static_cast<std::uint8_t>(ParseInteger(
            values, "MDVWB_MODBUS_STOP_BITS", 1, true)),
    };
    mdv::ValidateSerialSettings(runtime.serial);

    const int timeout = ParseInteger(
        values,
        "MDVWB_MODBUS_RESPONSE_TIMEOUT_MS",
        200);
    if (timeout <= 0 || timeout > 1000) {
        throw std::runtime_error(
            "managed Modbus discovery response timeout must be in range 1..1000 ms");
    }
    runtime.responseTimeout = std::chrono::milliseconds(timeout);

    return DiscoveryRuntimeSelection{
        .protocol = DiscoveryRuntimeProtocol::ModbusRtu,
        .modbus = std::move(runtime),
    };
}

[[nodiscard]] const mdv::modbus::ModbusProfile& ResolveProfile(
    const ModbusDiscoveryRuntime& runtime,
    const mdv::modbus::ProfileCatalog& catalog)
{
    const mdv::modbus::ModbusProfile* profile =
        catalog.Find(runtime.profileId);
    if (profile == nullptr) {
        throw std::runtime_error(
            "selected Modbus discovery profile '" + runtime.profileId +
            "' is unavailable");
    }

    const auto& expected = profile->transport;
    const auto& actual = runtime.serial;
    if (expected.baudRate != actual.baudRate ||
        expected.dataBits != actual.dataBits ||
        expected.parity != actual.parity ||
        expected.stopBits != actual.stopBits) {
        throw std::runtime_error(
            "managed Modbus discovery serial settings do not match profile '" +
            runtime.profileId + "'");
    }
    return *profile;
}

[[nodiscard]] std::string JoinAddresses(const std::vector<int>& addresses)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << addresses[index];
    }
    return output.str();
}

[[nodiscard]] std::string ScanReasonText(mdv::modbus::ScanReason reason)
{
    using mdv::modbus::ScanReason;
    switch (reason) {
    case ScanReason::Success: return "success";
    case ScanReason::UnsupportedCandidate: return "unsupported-candidate";
    case ScanReason::UnsupportedDataSpace: return "unsupported-data-space";
    case ScanReason::Timeout: return "timeout";
    case ScanReason::ExceptionResponse: return "exception-response";
    case ScanReason::PresenceMismatch: return "presence-mismatch";
    case ScanReason::InvalidResponse: return "invalid-response";
    case ScanReason::IoError: return "io-error";
    case ScanReason::InvalidRequest: return "invalid-request";
    }
    return "unknown";
}

} // namespace

std::optional<DiscoveryRuntimeSelection> FindDiscoveryRuntimeForPort(
    const std::filesystem::path& defaultDirectory,
    std::string_view port)
{
    if (port.empty()) {
        throw std::invalid_argument("discovery port cannot be empty");
    }

    std::error_code error;
    if (!std::filesystem::exists(defaultDirectory, error)) {
        if (error) {
            throw std::runtime_error(
                "cannot inspect managed discovery directory: " +
                error.message());
        }
        return std::nullopt;
    }
    if (!std::filesystem::is_directory(defaultDirectory, error)) {
        if (error) {
            throw std::runtime_error(
                "cannot inspect managed discovery directory: " +
                error.message());
        }
        throw std::runtime_error(
            "managed discovery path is not a directory");
    }

    std::vector<std::filesystem::path> candidates;
    for (std::filesystem::directory_iterator iterator(defaultDirectory, error), end;
         iterator != end;
         iterator.increment(error)) {
        if (error) {
            throw std::runtime_error(
                "cannot enumerate managed discovery directory: " +
                error.message());
        }
        const auto& entry = *iterator;
        if (!entry.is_regular_file(error)) {
            error.clear();
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (name.starts_with("mdvwb-")) {
            candidates.push_back(entry.path());
        }
    }
    if (error) {
        throw std::runtime_error(
            "cannot enumerate managed discovery directory: " +
            error.message());
    }
    std::sort(candidates.begin(), candidates.end());

    std::optional<DiscoveryRuntimeSelection> selected;
    for (const auto& candidate : candidates) {
        const auto values = ReadManagedAssignments(candidate);
        if (values.empty()) {
            continue;
        }
        const auto portIterator = values.find("MDVWB_PORT");
        if (portIterator == values.end() || portIterator->second != port) {
            continue;
        }
        if (selected.has_value()) {
            throw std::runtime_error(
                "multiple managed bus configurations use discovery port '" +
                std::string(port) + "'");
        }
        selected = SelectionFromAssignments(values, std::string(port));
    }
    return selected;
}

std::filesystem::path DiscoveryDefaultDirectoryFromEnvironment()
{
    const char* value = std::getenv("MDVWB_DEFAULT_DIR");
    if (value != nullptr && value[0] != '\0') {
        return value;
    }
    return "/etc/default";
}

ModbusDiscoveryScanResult ExecuteModbusDiscovery(
    const ModbusDiscoveryRuntime& runtime,
    mdv::modbus::ITransactionTransport& transport)
{
    if (runtime.port.empty()) {
        throw std::invalid_argument("Modbus discovery port cannot be empty");
    }
    if (runtime.profileDirectory.empty() || runtime.profileId.empty()) {
        throw std::invalid_argument(
            "Modbus discovery requires a profile directory and profile ID");
    }

    mdv::modbus::ProfileCatalog catalog;
    try {
        catalog = mdv::modbus::LoadProfileDirectory(runtime.profileDirectory);
    }
    catch (const std::exception&) {
        throw std::runtime_error("Modbus profile directory is unavailable");
    }
    const auto& profile = ResolveProfile(runtime, catalog);

    const mdv::modbus::ScanPlan plan =
        mdv::modbus::BuildScanPlan(profile);

    ModbusDiscoveryScanResult result;
    std::ostringstream details;
    for (const auto& candidate : plan) {
        if (!candidate.probe.has_value()) {
            details << "MODBUS_SCAN address="
                    << static_cast<int>(candidate.logicalAddress)
                    << " result=unsupported-candidate\n";
            continue;
        }

        const mdv::modbus::ScanResult item =
            mdv::modbus::ExecuteScanProbe(*candidate.probe, transport);
        details << "MODBUS_SCAN address="
                << static_cast<int>(item.logicalAddress)
                << " result=" << ScanReasonText(item.reason) << '\n';
        if (item.disposition == mdv::modbus::ScanDisposition::Found) {
            result.addresses.push_back(static_cast<int>(item.logicalAddress));
        }
        else if (item.disposition == mdv::modbus::ScanDisposition::Error) {
            result.addresses.clear();
            result.output = details.str();
            result.message =
                "Modbus discovery failed at logical address " +
                std::to_string(item.logicalAddress) +
                (item.diagnostic.empty()
                    ? std::string{}
                    : ": " + item.diagnostic);
            result.output += "FOUND_ADDRESSES=\n";
            return result;
        }
    }

    result.success = true;
    result.output = details.str();
    result.output += "FOUND_ADDRESSES=" + JoinAddresses(result.addresses) + "\n";
    result.message = result.addresses.empty()
        ? "Discovery completed; no devices found"
        : "Discovery completed";
    return result;
}

ModbusDiscoveryScanResult RunModbusDiscovery(
    const ModbusDiscoveryRuntime& runtime)
{
    mdv::modbus::RtuSerialTransport transport(
        runtime.serial,
        mdv::modbus::RtuTimingSettings{
            .responseTimeout = runtime.responseTimeout,
        });
    transport.Open(runtime.port);
    return ExecuteModbusDiscovery(runtime, transport);
}

} // namespace mdvwb
