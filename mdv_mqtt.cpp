#include "mdv_mqtt.h"

#include <charconv>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mdv {
namespace {

[[nodiscard]] std::string_view Trim(std::string_view value) noexcept
{
    while (!value.empty() &&
           (value.front() == ' ' || value.front() == '\t' ||
            value.front() == '\r' || value.front() == '\n')) {
        value.remove_prefix(1);
    }
    while (!value.empty() &&
           (value.back() == ' ' || value.back() == '\t' ||
            value.back() == '\r' || value.back() == '\n')) {
        value.remove_suffix(1);
    }
    return value;
}

[[nodiscard]] std::optional<int> ParseInteger(std::string_view payload) noexcept
{
    payload = Trim(payload);
    if (payload.empty()) {
        return std::nullopt;
    }

    int value = 0;
    const auto result = std::from_chars(
        payload.data(), payload.data() + payload.size(), value);
    if (result.ec != std::errc{} || result.ptr != payload.data() + payload.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::vector<std::string_view> SplitTopic(std::string_view topic)
{
    std::vector<std::string_view> parts;
    if (topic.empty() || topic.front() != '/') {
        return parts;
    }

    topic.remove_prefix(1);
    while (true) {
        const auto separator = topic.find('/');
        if (separator == std::string_view::npos) {
            parts.push_back(topic);
            break;
        }
        parts.push_back(topic.substr(0, separator));
        topic.remove_prefix(separator + 1);
    }
    return parts;
}

struct ParsedDeviceName {
    int bus = 0;
    int address = 0;
};

[[nodiscard]] std::optional<ParsedDeviceName> ParseDeviceName(
    std::string_view value) noexcept
{
    constexpr std::string_view prefix = "Fan-";
    if (!value.starts_with(prefix)) {
        return std::nullopt;
    }
    value.remove_prefix(prefix.size());

    const auto separator = value.find('_');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1 >= value.size()) {
        return std::nullopt;
    }

    const auto busText = value.substr(0, separator);
    const auto addressText = value.substr(separator + 1);

    int bus = 0;
    int address = 0;
    const auto busResult = std::from_chars(
        busText.data(), busText.data() + busText.size(), bus);
    const auto addressResult = std::from_chars(
        addressText.data(), addressText.data() + addressText.size(), address);

    if (busResult.ec != std::errc{} ||
        busResult.ptr != busText.data() + busText.size() ||
        addressResult.ec != std::errc{} ||
        addressResult.ptr != addressText.data() + addressText.size()) {
        return std::nullopt;
    }

    if (bus < 0 || address < 0 || address > kMaxDeviceAddress) {
        return std::nullopt;
    }
    return ParsedDeviceName{bus, address};
}

[[nodiscard]] Mode ModeFromMqtt(int value)
{
    switch (value) {
    case 0:
        return Mode::Cool;
    case 1:
        return Mode::Heat;
    case 2:
        return Mode::Dry;
    case 3:
        return Mode::Fan;
    case 4:
        return Mode::Auto;
    default:
        throw std::invalid_argument("Mode must be in range 0..4");
    }
}

[[nodiscard]] FanSpeed SpeedFromMqtt(int value)
{
    switch (value) {
    case 1:
        return FanSpeed::Low;
    case 2:
        return FanSpeed::Medium;
    case 3:
        return FanSpeed::High;
    case 4:
        return FanSpeed::Auto;
    default:
        throw std::invalid_argument("Speed must be in range 1..4");
    }
}

[[nodiscard]] bool BoolFromMqtt(int value, std::string_view control)
{
    if (value == 0) {
        return false;
    }
    if (value == 1) {
        return true;
    }
    throw std::invalid_argument(std::string(control) + " must be 0 or 1");
}

} // namespace

void MqttCommandInbox::Push(MqttMessage message)
{
    std::lock_guard lock(mutex_);
    messages_.push_back(std::move(message));
}

std::optional<MqttMessage> MqttCommandInbox::TryPop()
{
    std::lock_guard lock(mutex_);
    if (messages_.empty()) {
        return std::nullopt;
    }

    auto message = std::move(messages_.front());
    messages_.pop_front();
    return message;
}

std::size_t MqttCommandInbox::Size() const
{
    std::lock_guard lock(mutex_);
    return messages_.size();
}

MqttCommandRouter::MqttCommandRouter(int busNumber, MdvDriver& driver)
    : busNumber_(busNumber), driver_(driver)
{
    if (busNumber_ < 0) {
        throw std::invalid_argument("MQTT bus number cannot be negative");
    }
}

std::string_view MqttCommandRouter::SubscriptionTopic() noexcept
{
    return "/devices/+/controls/+/on1";
}

MqttCommandResult MqttCommandRouter::Handle(const MqttMessage& message)
{
    if (message.retained) {
        return MqttCommandResult{
            .status = MqttCommandStatus::InvalidPayload,
            .address = std::nullopt,
            .control = {},
            .error = "retained MQTT commands are ignored for safety",
        };
    }

    const auto parts = SplitTopic(message.topic);
    if (parts.size() != 5 || parts[0] != "devices" ||
        parts[2] != "controls" || parts[4] != "on1") {
        return MqttCommandResult{
            .status = MqttCommandStatus::InvalidTopic,
            .address = std::nullopt,
            .control = {},
            .error = "MQTT command topic must match /devices/Fan-X_Y/controls/Control/on1",
        };
    }

    const auto deviceName = ParseDeviceName(parts[1]);
    if (!deviceName.has_value()) {
        return MqttCommandResult{
            .status = MqttCommandStatus::InvalidTopic,
            .address = std::nullopt,
            .control = {},
            .error = "MQTT device name must match Fan-<bus>_<address>",
        };
    }

    if (deviceName->bus != busNumber_) {
        return MqttCommandResult{
            .status = MqttCommandStatus::Ignored,
            .address = static_cast<std::uint8_t>(deviceName->address),
            .control = std::string(parts[3]),
            .error = {},
        };
    }

    const auto value = ParseInteger(message.payload);
    if (!value.has_value()) {
        return MqttCommandResult{
            .status = MqttCommandStatus::InvalidPayload,
            .address = static_cast<std::uint8_t>(deviceName->address),
            .control = std::string(parts[3]),
            .error = "MQTT payload must contain one integer value",
        };
    }

    return Apply(
        static_cast<std::uint8_t>(deviceName->address), parts[3], *value);
}

MqttCommandResult MqttCommandRouter::Apply(
    std::uint8_t address,
    std::string_view control,
    int value)
{
    MqttCommandResult result;
    result.address = address;
    result.control = std::string(control);

    try {
        if (control == "Power") {
            driver_.SetPower(address, BoolFromMqtt(value, control));
        }
        else if (control == "Mode") {
            driver_.SetMode(address, ModeFromMqtt(value));
        }
        else if (control == "Speed") {
            driver_.SetFanSpeed(address, SpeedFromMqtt(value));
        }
        else if (control == "SetTemp") {
            if (value < 16 || value > 32) {
                throw std::invalid_argument("SetTemp must be in range 16..32");
            }
            driver_.SetTemperature(address, static_cast<std::uint8_t>(value));
        }
        else if (control == "Blinds") {
            driver_.SetBlinds(address, BoolFromMqtt(value, control));
        }
        else if (control == "Blok") {
            driver_.SetBlocked(address, BoolFromMqtt(value, control));
        }
        else {
            result.status = MqttCommandStatus::InvalidTopic;
            result.error = "unsupported MQTT control";
            return result;
        }

        result.status = MqttCommandStatus::Applied;
        return result;
    }
    catch (const std::out_of_range& error) {
        result.status = MqttCommandStatus::DeviceNotConfigured;
        result.error = error.what();
        return result;
    }
    catch (const std::invalid_argument& error) {
        result.status = MqttCommandStatus::InvalidPayload;
        result.error = error.what();
        return result;
    }
    catch (const std::logic_error& error) {
        result.status = MqttCommandStatus::DeviceNotInitialized;
        result.error = error.what();
        return result;
    }
}

MqttCommandService::MqttCommandService(
    IMqttClient& client,
    MqttCommandRouter& router)
    : client_(client), router_(router)
{
}

void MqttCommandService::Start()
{
    if (started_) {
        return;
    }

    client_.SetMessageHandler([this](MqttMessage message) {
        inbox_.Push(std::move(message));
    });
    client_.Subscribe(MqttCommandRouter::SubscriptionTopic());
    started_ = true;
}

std::optional<MqttCommandResult> MqttCommandService::ProcessOne()
{
    const auto message = inbox_.TryPop();
    if (!message.has_value()) {
        return std::nullopt;
    }
    return router_.Handle(*message);
}

std::size_t MqttCommandService::PendingCount() const
{
    return inbox_.Size();
}

} // namespace mdv
