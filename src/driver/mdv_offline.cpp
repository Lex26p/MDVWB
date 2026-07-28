#include "mdv_mosquitto.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr int kMaximumBusId = 999;
constexpr int kMaximumAddress = 63;
constexpr auto kConnectionTimeout = std::chrono::seconds(5);
constexpr auto kDeliveryTimeout = std::chrono::seconds(5);
constexpr auto kDeliverySettleTime = std::chrono::milliseconds(500);
constexpr auto kWaitStep = std::chrono::milliseconds(20);

struct OfflineConfig {
    std::vector<std::uint8_t> addresses;
    int busNumber = 0;
    mdv::MqttConnectionOptions mqtt;
};

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
    const int value = ParseInteger(text, optionName);
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
        const int address = ParseInteger(token, "device address");
        if (address < 0 || address > kMaximumAddress) {
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
    return addresses;
}

[[nodiscard]] std::string_view RequireValue(
    const std::vector<std::string_view>& arguments,
    std::size_t& index,
    std::string_view option)
{
    if (index + 1U >= arguments.size()) {
        throw std::invalid_argument(
            std::string(option) + " requires a value");
    }
    ++index;
    return arguments[index];
}

[[nodiscard]] OfflineConfig ParseArguments(
    const std::vector<std::string_view>& arguments)
{
    OfflineConfig config;
    config.mqtt.clientId.clear();

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const auto option = arguments[index];
        if (option == "--addresses") {
            config.addresses = ParseAddresses(
                RequireValue(arguments, index, option));
        }
        else if (option == "--bus") {
            config.busNumber = ParseInteger(
                RequireValue(arguments, index, option), option);
        }
        else if (option == "--mqtt-host") {
            config.mqtt.host = RequireValue(arguments, index, option);
        }
        else if (option == "--mqtt-port") {
            config.mqtt.port = ParseInteger(
                RequireValue(arguments, index, option), option);
        }
        else if (option == "--mqtt-user") {
            config.mqtt.username = RequireValue(arguments, index, option);
        }
        else if (option == "--mqtt-password") {
            config.mqtt.password = RequireValue(arguments, index, option);
        }
        else if (option == "--mqtt-client-id") {
            config.mqtt.clientId = RequireValue(arguments, index, option);
        }
        else if (option == "--mqtt-keepalive") {
            config.mqtt.keepAliveSeconds = ParseInteger(
                RequireValue(arguments, index, option), option);
        }
        else if (option == "--mqtt-reconnect") {
            config.mqtt.reconnectDelaySeconds = ParseUnsigned(
                RequireValue(arguments, index, option), option);
        }
        else if (option == "--mqtt-reconnect-max") {
            config.mqtt.reconnectDelayMaxSeconds = ParseUnsigned(
                RequireValue(arguments, index, option), option);
        }
        else {
            throw std::invalid_argument(
                "unknown command-line option: " + std::string(option));
        }
    }

    if (config.addresses.empty()) {
        throw std::invalid_argument("at least one fan coil address is required");
    }
    if (config.busNumber < 1 || config.busNumber > kMaximumBusId) {
        throw std::invalid_argument("bus number must be in range 1..999");
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
        config.mqtt.clientId =
            "mdvwb-offline-" + std::to_string(config.busNumber);
    }
    return config;
}

[[nodiscard]] std::string FanTopic(
    int busNumber,
    std::uint8_t address,
    std::string_view control)
{
    return "/devices/Fan-" + std::to_string(busNumber) + "_" +
        std::to_string(static_cast<int>(address)) + "/controls/" +
        std::string(control);
}

[[nodiscard]] std::vector<mdv::MqttPublication> BuildOfflinePublications(
    const OfflineConfig& config)
{
    std::vector<mdv::MqttPublication> publications;
    publications.reserve(config.addresses.size() * 2U + 1U);
    for (const auto address : config.addresses) {
        publications.push_back({
            FanTopic(config.busNumber, address, "Alarm"), "2", true});
        publications.push_back({
            FanTopic(config.busNumber, address, "Status"), "7", true});
    }
    publications.push_back({
        "/devices/sist-" + std::to_string(config.busNumber) +
            "/controls/Serial",
        "Порт закрыт",
        true,
    });
    return publications;
}

[[nodiscard]] bool WaitForConnection(
    mdv::MosquittoMqttClient& client,
    std::chrono::steady_clock::duration timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (client.IsConnected()) {
            return true;
        }
        std::this_thread::sleep_for(kWaitStep);
    }
    return client.IsConnected();
}

[[nodiscard]] bool WaitForDelivery(
    mdv::MosquittoMqttClient& client,
    std::chrono::steady_clock::duration timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (client.IsConnected() && client.PendingPublicationCount() == 0U) {
            std::this_thread::sleep_for(kDeliverySettleTime);
            return client.IsConnected() &&
                client.PendingPublicationCount() == 0U;
        }
        std::this_thread::sleep_for(kWaitStep);
    }
    return false;
}

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

int RunSelfTest()
{
    const auto config = ParseArguments({
        "--addresses", "1,63",
        "--bus", "4",
        "--mqtt-host", "127.0.0.1",
        "--mqtt-port", "1883",
        "--mqtt-user", "user",
        "--mqtt-password", "secret",
    });
    const auto publications = BuildOfflinePublications(config);

    Require(config.addresses == std::vector<std::uint8_t>({1, 63}),
            "offline address parsing");
    Require(config.busNumber == 4,
            "offline bus parsing");
    Require(config.mqtt.clientId == "mdvwb-offline-4",
            "offline client ID");
    Require(publications.size() == 5U,
            "offline publication count");
    Require(publications[0].topic ==
                "/devices/Fan-4_1/controls/Alarm" &&
            publications[0].payload == "2" && publications[0].retained,
            "offline Alarm publication");
    Require(publications[1].topic ==
                "/devices/Fan-4_1/controls/Status" &&
            publications[1].payload == "7" && publications[1].retained,
            "offline Status publication");
    Require(publications[4].topic ==
                "/devices/sist-4/controls/Serial" &&
            publications[4].payload == "Порт закрыт" &&
            publications[4].retained,
            "offline serial publication");
    Require(std::all_of(
                publications.begin(), publications.end(),
                [](const mdv::MqttPublication& publication) {
                    return publication.retained;
                }),
            "all offline publications retained");

    bool duplicateRejected = false;
    bool addressRejected = false;
    bool busRejected = false;
    bool passwordRejected = false;
    try {
        static_cast<void>(ParseArguments({
            "--addresses", "1,1", "--bus", "1"}));
    }
    catch (const std::invalid_argument&) {
        duplicateRejected = true;
    }
    try {
        static_cast<void>(ParseArguments({
            "--addresses", "64", "--bus", "1"}));
    }
    catch (const std::invalid_argument&) {
        addressRejected = true;
    }
    try {
        static_cast<void>(ParseArguments({
            "--addresses", "1", "--bus", "0"}));
    }
    catch (const std::invalid_argument&) {
        busRejected = true;
    }
    try {
        static_cast<void>(ParseArguments({
            "--addresses", "1", "--bus", "1",
            "--mqtt-password", "secret"}));
    }
    catch (const std::invalid_argument&) {
        passwordRejected = true;
    }

    Require(duplicateRejected, "duplicate offline address rejected");
    Require(addressRejected, "invalid offline address rejected");
    Require(busRejected, "invalid offline bus rejected");
    Require(passwordRejected, "offline MQTT password without user rejected");

    std::cout << "MDVWB offline publisher self-test: OK\n";
    return 0;
}

[[nodiscard]] std::string HelpText(std::string_view executableName)
{
    return std::string(executableName) +
        " --addresses LIST --bus NUMBER [MQTT options]\n"
        "Publishes retained Alarm=2 and Status=7 after a bus driver stops.\n";
}

int RunOfflinePublisher(const OfflineConfig& config)
{
    if (!mdv::MosquittoMqttClient::IsSupported()) {
        throw std::runtime_error(
            "libmosquitto support is not available in this build");
    }

    mdv::MosquittoMqttClient client(config.mqtt);
    client.Start();
    if (!WaitForConnection(client, kConnectionTimeout)) {
        const auto error = client.LastError();
        client.Stop();
        throw std::runtime_error(
            error.empty() ? "cannot connect to MQTT broker" : error);
    }

    bool accepted = true;
    for (const auto& publication : BuildOfflinePublications(config)) {
        const auto status = client.PublishWithResult(
            publication.topic, publication.payload, publication.retained);
        accepted = accepted &&
            (status == mdv::MqttPublishStatus::Published ||
             status == mdv::MqttPublishStatus::QueuedRetained);
    }

    const bool delivered = accepted &&
        WaitForDelivery(client, kDeliveryTimeout);
    const auto error = client.LastError();
    client.Stop();
    if (!delivered) {
        throw std::runtime_error(
            error.empty()
                ? "offline MQTT states were not delivered before timeout"
                : error);
    }
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--self-test") {
            return RunSelfTest();
        }
        if (argc == 2 &&
            (std::string_view(argv[1]) == "--help" ||
             std::string_view(argv[1]) == "-h")) {
            std::cout << HelpText(argc > 0 ? argv[0] : "mdvwb-offline");
            return 0;
        }

        std::vector<std::string_view> arguments;
        arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
        for (int index = 1; index < argc; ++index) {
            arguments.emplace_back(argv[index]);
        }
        return RunOfflinePublisher(ParseArguments(arguments));
    }
    catch (const std::invalid_argument& error) {
        std::cerr << "Offline publisher configuration error: "
                  << error.what() << '\n';
        return 2;
    }
    catch (const std::exception& error) {
        std::cerr << "Offline publisher runtime error: "
                  << error.what() << '\n';
        return 3;
    }
}
