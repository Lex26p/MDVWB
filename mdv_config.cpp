#include "mdv_config.h"

#include "mdv_protocol.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace mdv {
namespace {

[[nodiscard]] int ParseInteger(
    std::string_view text,
    std::string_view optionName)
{
    if (text.empty()) {
        throw std::invalid_argument(std::string(optionName) + " cannot be empty");
    }

    int value = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::invalid_argument(
            std::string(optionName) + " must be an integer");
    }
    return value;
}

[[nodiscard]] unsigned int ParseUnsigned(
    std::string_view text,
    std::string_view optionName)
{
    const auto value = ParseInteger(text, optionName);
    if (value <= 0) {
        throw std::invalid_argument(
            std::string(optionName) + " must be positive");
    }
    return static_cast<unsigned int>(value);
}

[[nodiscard]] std::vector<std::uint8_t> ParseAddresses(
    std::string_view text)
{
    if (text.empty()) {
        throw std::invalid_argument("address list cannot be empty");
    }

    std::vector<std::uint8_t> addresses;
    std::unordered_set<int> unique;
    std::size_t begin = 0;

    while (begin <= text.size()) {
        const auto separator = text.find(',', begin);
        const auto end = separator == std::string_view::npos
            ? text.size()
            : separator;
        const auto token = text.substr(begin, end - begin);
        const auto address = ParseInteger(token, "device address");
        if (address < 0 || address > kMaxDeviceAddress) {
            throw std::invalid_argument(
                "device address must be in range 0..63");
        }
        if (!unique.insert(address).second) {
            throw std::invalid_argument(
                "device address list contains duplicates");
        }
        addresses.push_back(static_cast<std::uint8_t>(address));

        if (separator == std::string_view::npos) {
            break;
        }
        begin = separator + 1;
    }

    if (addresses.empty()) {
        throw std::invalid_argument("address list cannot be empty");
    }
    return addresses;
}

void ValidateConfig(ApplicationConfig& config)
{
    if (config.addresses.empty()) {
        throw std::invalid_argument("at least one fan coil address is required");
    }
    if (config.serialPort.empty()) {
        throw std::invalid_argument("serial port cannot be empty");
    }
    if (config.busNumber < 0) {
        throw std::invalid_argument("bus number cannot be negative");
    }
    if (config.masterId > kMaxDeviceAddress) {
        throw std::invalid_argument("master ID must be in range 0..63");
    }
    if (config.timing.transactionPeriod.count() <= 0) {
        throw std::invalid_argument("transaction period must be positive");
    }
    if (config.timing.responseTimeout.count() <= 0 ||
        config.timing.responseTimeout >= config.timing.transactionPeriod) {
        throw std::invalid_argument(
            "response timeout must be positive and shorter than transaction period");
    }
    if (config.mqtt.host.empty()) {
        throw std::invalid_argument("MQTT host cannot be empty");
    }
    if (config.mqtt.port <= 0 || config.mqtt.port > 65535) {
        throw std::invalid_argument("MQTT port must be in range 1..65535");
    }
    if (config.mqtt.keepAliveSeconds <= 0) {
        throw std::invalid_argument("MQTT keepalive must be positive");
    }
    if (config.mqtt.username.empty() && !config.mqtt.password.empty()) {
        throw std::invalid_argument("MQTT password requires a username");
    }
    if (config.mqtt.reconnectDelaySeconds == 0 ||
        config.mqtt.reconnectDelayMaxSeconds <
            config.mqtt.reconnectDelaySeconds) {
        throw std::invalid_argument("invalid MQTT reconnect delay range");
    }
    if (config.mqtt.clientId.empty()) {
        config.mqtt.clientId = "mdvwb-" + std::to_string(config.busNumber);
    }
}

[[nodiscard]] std::string_view RequireValue(
    int argc,
    char* argv[],
    int& index,
    std::string_view option)
{
    if (index + 1 >= argc) {
        throw std::invalid_argument(
            std::string(option) + " requires a value");
    }
    ++index;
    return argv[index];
}

[[nodiscard]] ApplicationConfig ParseLegacy(int argc, char* argv[])
{
    if (argc != 4) {
        throw std::invalid_argument(
            "legacy launch format requires: <addresses> <serial-port> <bus-number>");
    }

    ApplicationConfig config;
    config.addresses = ParseAddresses(argv[1]);
    config.serialPort = argv[2];
    config.busNumber = ParseInteger(argv[3], "bus number");
    config.mqtt.clientId.clear();
    ValidateConfig(config);
    return config;
}

[[nodiscard]] ApplicationConfig ParseNamed(int argc, char* argv[])
{
    ApplicationConfig config;
    config.mqtt.clientId.clear();

    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--addresses") {
            config.addresses = ParseAddresses(
                RequireValue(argc, argv, index, option));
        }
        else if (option == "--port") {
            config.serialPort = RequireValue(argc, argv, index, option);
        }
        else if (option == "--bus") {
            config.busNumber = ParseInteger(
                RequireValue(argc, argv, index, option), option);
        }
        else if (option == "--master-id") {
            const auto value = ParseInteger(
                RequireValue(argc, argv, index, option), option);
            if (value < 0 || value > kMaxDeviceAddress) {
                throw std::invalid_argument("--master-id must be in range 0..63");
            }
            config.masterId = static_cast<std::uint8_t>(value);
        }
        else if (option == "--period-ms") {
            config.timing.transactionPeriod = std::chrono::milliseconds(
                ParseInteger(RequireValue(argc, argv, index, option), option));
        }
        else if (option == "--response-timeout-ms") {
            config.timing.responseTimeout = std::chrono::milliseconds(
                ParseInteger(RequireValue(argc, argv, index, option), option));
        }
        else if (option == "--mqtt-host") {
            config.mqtt.host = RequireValue(argc, argv, index, option);
        }
        else if (option == "--mqtt-port") {
            config.mqtt.port = ParseInteger(
                RequireValue(argc, argv, index, option), option);
        }
        else if (option == "--mqtt-user") {
            config.mqtt.username = RequireValue(argc, argv, index, option);
        }
        else if (option == "--mqtt-password") {
            config.mqtt.password = RequireValue(argc, argv, index, option);
        }
        else if (option == "--mqtt-client-id") {
            config.mqtt.clientId = RequireValue(argc, argv, index, option);
        }
        else if (option == "--mqtt-keepalive") {
            config.mqtt.keepAliveSeconds = ParseInteger(
                RequireValue(argc, argv, index, option), option);
        }
        else if (option == "--mqtt-reconnect") {
            config.mqtt.reconnectDelaySeconds = ParseUnsigned(
                RequireValue(argc, argv, index, option), option);
        }
        else if (option == "--mqtt-reconnect-max") {
            config.mqtt.reconnectDelayMaxSeconds = ParseUnsigned(
                RequireValue(argc, argv, index, option), option);
        }
        else if (option == "--publish-poll-address") {
            config.publishPollAddress = true;
        }
        else {
            throw std::invalid_argument(
                "unknown command-line option: " + std::string(option));
        }
    }

    ValidateConfig(config);
    return config;
}

} // namespace

CommandLineResult ParseCommandLine(int argc, char* argv[])
{
    if (argc <= 1) {
        return CommandLineResult{.action = CommandLineAction::Help};
    }

    const std::string_view first = argv[1];
    if (first == "--self-test") {
        if (argc != 2) {
            throw std::invalid_argument("--self-test does not accept other arguments");
        }
        return CommandLineResult{.action = CommandLineAction::SelfTest};
    }
    if (first == "--help" || first == "-h") {
        if (argc != 2) {
            throw std::invalid_argument("--help does not accept other arguments");
        }
        return CommandLineResult{.action = CommandLineAction::Help};
    }

    CommandLineResult result;
    result.action = CommandLineAction::Run;
    result.config = first.starts_with("--")
        ? ParseNamed(argc, argv)
        : ParseLegacy(argc, argv);
    return result;
}

std::string BuildHelpText(std::string_view executableName)
{
    std::ostringstream output;
    output
        << "MDVWB - individual MDV fan-coil driver\n\n"
        << "Legacy launch format:\n  " << executableName
        << " 1,2,3 COM4 1\n  " << executableName
        << " 1,2,3 /dev/ttyRS485-1 1\n\n"
        << "Named options:\n  " << executableName
        << " --addresses 1,2,3 --port COM4 --bus 1 [options]\n\n"
        << "Required options:\n"
        << "  --addresses LIST          Comma-separated addresses 0..63\n"
        << "  --port NAME               COM port or /dev/ttyRS485-*\n"
        << "  --bus NUMBER              MQTT device bus number\n\n"
        << "MDV options:\n"
        << "  --master-id NUMBER        Master ID 0..63 (default 0)\n"
        << "  --period-ms NUMBER        Start-to-start period (default 150)\n"
        << "  --response-timeout-ms N   Response timeout (default 130)\n\n"
        << "MQTT options:\n"
        << "  --mqtt-host HOST          Broker host (default 127.0.0.1)\n"
        << "  --mqtt-port PORT          Broker port (default 1883)\n"
        << "  --mqtt-user USER          Optional username\n"
        << "  --mqtt-password PASSWORD  Optional password\n"
        << "  --mqtt-client-id ID       Default mdvwb-<bus>\n"
        << "  --mqtt-keepalive SEC      Default 60\n"
        << "  --mqtt-reconnect SEC      Default 1\n"
        << "  --mqtt-reconnect-max SEC  Default 10\n\n"
        << "Diagnostics:\n"
        << "  --publish-poll-address    Publish sist-<bus>/GanGetID each slot\n\n"
        << "Other:\n"
        << "  --self-test               Run internal tests\n"
        << "  --help, -h                Show this help\n";
    return output.str();
}

std::string FormatAddressList(const std::vector<std::uint8_t>& addresses)
{
    std::ostringstream output;
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << static_cast<int>(addresses[index]);
    }
    return output.str();
}

} // namespace mdv
