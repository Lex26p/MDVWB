#include "modbus_runtime_config.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace mdv::modbus {
namespace {

[[noreturn]] void Fail(std::string message)
{
    throw std::invalid_argument(std::move(message));
}

[[nodiscard]] std::optional<std::string> OptionalValue(
    const EnvironmentLookup& lookup,
    std::string_view name)
{
    const auto value = lookup(name);
    if (!value.has_value() || value->empty()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string RequireValue(
    const EnvironmentLookup& lookup,
    std::string_view name)
{
    const auto value = OptionalValue(lookup, name);
    if (!value.has_value()) {
        Fail("required runtime variable " + std::string(name) + " is missing");
    }
    return *value;
}

[[nodiscard]] int ParseInteger(
    std::string_view text,
    std::string_view name)
{
    int result = 0;
    const auto parsed = std::from_chars(
        text.data(),
        text.data() + text.size(),
        result);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size()) {
        Fail(std::string(name) + " must be an integer");
    }
    return result;
}

[[nodiscard]] int IntegerValue(
    const EnvironmentLookup& lookup,
    std::string_view name,
    int defaultValue)
{
    const auto value = OptionalValue(lookup, name);
    return value.has_value()
        ? ParseInteger(*value, name)
        : defaultValue;
}

[[nodiscard]] int IntegerInRange(
    const EnvironmentLookup& lookup,
    std::string_view name,
    int defaultValue,
    int minimum,
    int maximum)
{
    const int value = IntegerValue(lookup, name, defaultValue);
    if (value < minimum || value > maximum) {
        Fail(
            std::string(name) + " must be in range " +
            std::to_string(minimum) + ".." + std::to_string(maximum));
    }
    return value;
}

[[nodiscard]] unsigned int PositiveUnsigned(
    int value,
    std::string_view name)
{
    if (value <= 0) {
        Fail(std::string(name) + " must be positive");
    }
    return static_cast<unsigned int>(value);
}

[[nodiscard]] std::vector<std::uint8_t> ParseAddresses(
    std::string_view text)
{
    if (text.empty()) {
        Fail("MDVWB_ADDRESSES cannot be empty");
    }

    std::vector<std::uint8_t> result;
    std::set<int> unique;
    std::size_t begin = 0;

    while (begin <= text.size()) {
        const auto separator = text.find(',', begin);
        const auto end = separator == std::string_view::npos
            ? text.size()
            : separator;
        const auto token = text.substr(begin, end - begin);
        const int address = ParseInteger(token, "MDVWB_ADDRESSES");
        if (address < static_cast<int>(kMinLogicalAddress) ||
            address > static_cast<int>(kMaxLogicalAddress)) {
            Fail("Modbus logical address must be in range 1..63");
        }
        if (!unique.insert(address).second) {
            Fail("MDVWB_ADDRESSES contains duplicate logical address " +
                 std::to_string(address));
        }
        result.push_back(static_cast<std::uint8_t>(address));

        if (separator == std::string_view::npos) {
            break;
        }
        begin = separator + 1U;
    }

    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] SerialParity ParseParity(std::string_view text)
{
    if (text == "none") {
        return SerialParity::None;
    }
    if (text == "even") {
        return SerialParity::Even;
    }
    if (text == "odd") {
        return SerialParity::Odd;
    }
    Fail("MDVWB_MODBUS_PARITY must be none, even or odd");
}

[[nodiscard]] bool ParseFlag(int value, std::string_view name)
{
    if (value == 0) {
        return false;
    }
    if (value == 1) {
        return true;
    }
    Fail(std::string(name) + " must be 0 or 1");
}

[[nodiscard]] bool IsSafeProfileId(std::string_view value)
{
    if (value.empty()) {
        return false;
    }
    return std::all_of(
        value.begin(),
        value.end(),
        [](char character) {
            const auto byte = static_cast<unsigned char>(character);
            return (byte >= 'a' && byte <= 'z') ||
                (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') ||
                character == '_' || character == '-';
        });
}

void ValidateMqtt(RuntimeMqttSettings& mqtt, int busNumber)
{
    if (mqtt.host.empty()) {
        Fail("MDVWB_MQTT_HOST cannot be empty");
    }
    if (mqtt.port < 1 || mqtt.port > 65535) {
        Fail("MDVWB_MQTT_PORT must be in range 1..65535");
    }
    if (mqtt.keepAliveSeconds <= 0) {
        Fail("MDVWB_MQTT_KEEPALIVE must be positive");
    }
    if (mqtt.username.empty() && !mqtt.password.empty()) {
        Fail("MDVWB_MQTT_PASSWORD requires MDVWB_MQTT_USER");
    }
    if (mqtt.reconnectDelaySeconds == 0U ||
        mqtt.reconnectDelayMaxSeconds < mqtt.reconnectDelaySeconds) {
        Fail("invalid MQTT reconnect delay range");
    }
    if (mqtt.clientId.empty()) {
        mqtt.clientId = "mdvwb-" + std::to_string(busNumber);
    }
}

void ValidateProfileTransport(
    const ModbusRuntimeConfig& config,
    const ModbusProfile& profile)
{
    if (config.serial.baudRate != profile.transport.baudRate ||
        config.serial.dataBits != profile.transport.dataBits ||
        config.serial.parity != profile.transport.parity ||
        config.serial.stopBits != profile.transport.stopBits) {
        Fail(
            "managed Modbus serial settings do not match selected profile '" +
            profile.id + "'");
    }
}

} // namespace

ModbusRuntimeConfig ParseModbusRuntimeConfig(
    const EnvironmentLookup& lookup)
{
    ModbusRuntimeConfig result;
    result.addresses = ParseAddresses(
        RequireValue(lookup, "MDVWB_ADDRESSES"));
    result.serialPort = RequireValue(lookup, "MDVWB_PORT");
    result.busNumber = IntegerValue(lookup, "MDVWB_BUS", 1);
    if (result.busNumber < 1 || result.busNumber > 999) {
        Fail("MDVWB_BUS must be in range 1..999");
    }

    result.profileDirectory =
        RequireValue(lookup, "MDVWB_MODBUS_PROFILE_DIR");
    result.profileId = RequireValue(lookup, "MDVWB_MODBUS_PROFILE");
    if (!IsSafeProfileId(result.profileId)) {
        Fail("MDVWB_MODBUS_PROFILE contains unsafe characters");
    }

    const int baudRate = IntegerValue(
        lookup,
        "MDVWB_MODBUS_BAUD_RATE",
        9600);
    const int dataBits = IntegerValue(
        lookup,
        "MDVWB_MODBUS_DATA_BITS",
        8);
    const int stopBits = IntegerValue(
        lookup,
        "MDVWB_MODBUS_STOP_BITS",
        1);

    if (baudRate <= 0 ||
        static_cast<unsigned long long>(baudRate) >
            std::numeric_limits<std::uint32_t>::max()) {
        Fail("MDVWB_MODBUS_BAUD_RATE is outside the supported range");
    }
    if (dataBits < 0 || dataBits > 255 ||
        stopBits < 0 || stopBits > 255) {
        Fail("Modbus serial width is outside the supported range");
    }

    result.serial = SerialSettings{
        .baudRate = static_cast<std::uint32_t>(baudRate),
        .dataBits = static_cast<std::uint8_t>(dataBits),
        .parity = ParseParity(
            OptionalValue(lookup, "MDVWB_MODBUS_PARITY")
                .value_or("none")),
        .stopBits = static_cast<std::uint8_t>(stopBits),
    };
    ValidateSerialSettings(result.serial);

    result.cadence.pollPeriod = std::chrono::milliseconds(
        IntegerInRange(lookup, "MDVWB_PERIOD_MS", 150, 1, 60000));
    result.cadence.commandPeriod = std::chrono::milliseconds(
        IntegerInRange(
            lookup,
            "MDVWB_MODBUS_COMMAND_PERIOD_MS",
            20,
            1,
            60000));
    result.cadence.retryPeriod = std::chrono::milliseconds(
        IntegerInRange(
            lookup,
            "MDVWB_MODBUS_RETRY_PERIOD_MS",
            500,
            1,
            60000));
    result.responseTimeout = std::chrono::milliseconds(
        IntegerInRange(
            lookup,
            "MDVWB_MODBUS_RESPONSE_TIMEOUT_MS",
            200,
            1,
            60000));

    result.driverPolicy.maxWriteAttempts = static_cast<std::uint32_t>(
        IntegerInRange(
            lookup,
            "MDVWB_MODBUS_WRITE_ATTEMPTS",
            static_cast<int>(kMaxModbusWriteAttempts),
            1,
            10));
    result.driverPolicy.maxConfirmationAttempts =
        static_cast<std::uint32_t>(IntegerInRange(
            lookup,
            "MDVWB_MODBUS_CONFIRMATION_ATTEMPTS",
            static_cast<int>(kMaxModbusConfirmationAttempts),
            1,
            10));
    result.driverPolicy.maxPriorityOperationsBeforePoll =
        static_cast<std::size_t>(IntegerInRange(
            lookup,
            "MDVWB_MODBUS_PRIORITY_BURST",
            static_cast<int>(kMaxModbusPriorityOperationsBeforePoll),
            1,
            64));

    result.mqtt.host = OptionalValue(lookup, "MDVWB_MQTT_HOST")
        .value_or("127.0.0.1");
    result.mqtt.port = IntegerValue(lookup, "MDVWB_MQTT_PORT", 1883);
    result.mqtt.keepAliveSeconds = IntegerValue(
        lookup,
        "MDVWB_MQTT_KEEPALIVE",
        60);
    result.mqtt.clientId = OptionalValue(
        lookup,
        "MDVWB_MQTT_CLIENT_ID").value_or("");
    result.mqtt.username = OptionalValue(
        lookup,
        "MDVWB_MQTT_USER").value_or("");
    result.mqtt.password = OptionalValue(
        lookup,
        "MDVWB_MQTT_PASSWORD").value_or("");
    result.mqtt.reconnectDelaySeconds = PositiveUnsigned(
        IntegerValue(lookup, "MDVWB_MQTT_RECONNECT", 1),
        "MDVWB_MQTT_RECONNECT");
    result.mqtt.reconnectDelayMaxSeconds = PositiveUnsigned(
        IntegerValue(lookup, "MDVWB_MQTT_RECONNECT_MAX", 10),
        "MDVWB_MQTT_RECONNECT_MAX");
    ValidateMqtt(result.mqtt, result.busNumber);

    result.publishPollAddress = ParseFlag(
        IntegerValue(lookup, "MDVWB_PUBLISH_POLL_ADDRESS", 0),
        "MDVWB_PUBLISH_POLL_ADDRESS");

    return result;
}

ModbusProfile LoadModbusRuntimeProfile(
    const ModbusRuntimeConfig& config)
{
    const auto catalog = LoadProfileDirectory(config.profileDirectory);
    const auto* profile = catalog.Find(config.profileId);
    if (profile == nullptr) {
        Fail(
            "selected Modbus profile '" + config.profileId +
            "' is not available in " + config.profileDirectory.string());
    }

    ValidateProfileTransport(config, *profile);
    return *profile;
}

} // namespace mdv::modbus
