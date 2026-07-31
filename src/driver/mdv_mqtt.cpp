#include "mdv_mqtt.h"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
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

    if (bus < 0 || address < 0 || address > kMaxLogicalDeviceAddress) {
        return std::nullopt;
    }
    return ParsedDeviceName{bus, address};
}

[[nodiscard]] HvacMode ModeFromMqtt(int value)
{
    switch (value) {
    case 0:
        return HvacMode::Cool;
    case 1:
        return HvacMode::Heat;
    case 2:
        return HvacMode::Dry;
    case 3:
        return HvacMode::Fan;
    case 4:
        return HvacMode::Auto;
    default:
        throw std::invalid_argument("Mode must be in range 0..4");
    }
}

[[nodiscard]] HvacFanSpeed SpeedFromMqtt(int value)
{
    switch (value) {
    case 1:
        return HvacFanSpeed::Low;
    case 2:
        return HvacFanSpeed::Medium;
    case 3:
        return HvacFanSpeed::High;
    case 4:
        return HvacFanSpeed::Auto;
    default:
        throw std::invalid_argument("Speed must be in range 1..4");
    }
}

[[nodiscard]] int ModeToMqtt(HvacMode mode)
{
    switch (mode) {
    case HvacMode::Cool:
        return 0;
    case HvacMode::Heat:
        return 1;
    case HvacMode::Dry:
        return 2;
    case HvacMode::Fan:
        return 3;
    case HvacMode::Auto:
        return 4;
    }
    throw std::invalid_argument("unknown HVAC mode");
}

[[nodiscard]] int SpeedToMqtt(HvacFanSpeed speed)
{
    switch (speed) {
    case HvacFanSpeed::Low:
        return 1;
    case HvacFanSpeed::Medium:
        return 2;
    case HvacFanSpeed::High:
        return 3;
    case HvacFanSpeed::Auto:
        return 4;
    }
    throw std::invalid_argument("unknown HVAC fan speed");
}

[[nodiscard]] int DeviceStatus(const DriverDeviceState& state)
{
    if (state.alarmCode != 0) {
        return 6;
    }
    if (!state.power) {
        return 0;
    }

    const auto mode = state.mode.value_or(HvacMode::Auto);
    switch (mode) {
    case HvacMode::Cool:
        return 1;
    case HvacMode::Heat:
        return 2;
    case HvacMode::Dry:
        return 3;
    case HvacMode::Fan:
        return 4;
    case HvacMode::Auto:
        return 5;
    }
    return 0;
}

[[nodiscard]] std::string FormatNumber(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << value;
    auto text = stream.str();
    if (text.ends_with(".0")) {
        text.resize(text.size() - 2);
    }
    return text;
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

MqttCommandRouter::MqttCommandRouter(int busNumber, IDeviceDriver& driver)
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
        DriverCommand command;
        command.address = address;

        if (control == "Power") {
            command.control = DriverControl::Power;
            command.value = BoolFromMqtt(value, control);
        }
        else if (control == "Mode") {
            command.control = DriverControl::Mode;
            command.value = ModeFromMqtt(value);
        }
        else if (control == "Speed") {
            command.control = DriverControl::FanSpeed;
            command.value = SpeedFromMqtt(value);
        }
        else if (control == "SetTemp") {
            if (value < 16 || value > 32) {
                throw std::invalid_argument("SetTemp must be in range 16..32");
            }
            command.control = DriverControl::SetTemperature;
            command.value = static_cast<double>(value);
        }
        else if (control == "Blinds") {
            command.control = DriverControl::Blinds;
            command.value = BoolFromMqtt(value, control);
        }
        else if (control == "Blok") {
            command.control = DriverControl::Blocked;
            command.value = BoolFromMqtt(value, control);
        }
        else {
            result.status = MqttCommandStatus::InvalidTopic;
            result.error = "unsupported MQTT control";
            return result;
        }

        driver_.ApplyCommand(command);
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

MqttStatePublisher::MqttStatePublisher(int busNumber, IMqttClient& client)
    : busNumber_(busNumber), client_(client)
{
    if (busNumber_ < 0) {
        throw std::invalid_argument("MQTT bus number cannot be negative");
    }
}

void MqttStatePublisher::PublishAfter(
    const IDeviceDriver& driver,
    const DriverResult& result)
{
    if (result.operation != DriverOperation::PollRead &&
        result.operation != DriverOperation::ConfirmRead) {
        return;
    }

    if (result.outcome == DriverOutcome::Success) {
        PublishDevice(driver.DeviceStateByAddress(result.address));
    }
    else {
        PublishOffline(result.address, false);
    }
}

void MqttStatePublisher::PublishDevice(
    const DriverDeviceState& state,
    bool force)
{
    const auto address = state.address;
    if (!state.online || !state.hasState) {
        PublishOffline(address, force);
        return;
    }

    auto& previous = published_[address];
    const auto alarm = state.alarmCode == 0 ? 0 : 1;

    PublishInteger(address, "Power", state.power ? 1 : 0, previous.power, force);
    if (state.mode.has_value()) {
        PublishInteger(
            address, "Mode", ModeToMqtt(*state.mode), previous.mode, force);
    }
    if (state.fanSpeed.has_value()) {
        PublishInteger(
            address, "Speed", SpeedToMqtt(*state.fanSpeed), previous.speed, force);
    }
    if (state.setTemperature.has_value()) {
        PublishNumber(
            address, "SetTemp", *state.setTemperature,
            previous.setTemperature, force);
    }
    if (state.roomTemperature.has_value()) {
        PublishNumber(
            address, "Temp", *state.roomTemperature,
            previous.roomTemperature, force);
    }
    else if (mqtt_detail::ShouldPublishUnavailableNumber(
                 previous.roomTemperature, force)) {
        // An empty retained payload removes the obsolete numeric state from the
        // broker. Current subscribers also receive the empty payload and render
        // the room temperature as unavailable instead of keeping an old value.
        client_.Publish(Topic(address, "Temp"), "", true);
        previous.roomTemperature = mqtt_detail::UnavailableNumberMarker();
    }
    if (state.blinds.has_value()) {
        PublishInteger(
            address, "Blinds", *state.blinds ? 1 : 0, previous.blinds, force);
    }
    if (state.blocked.has_value()) {
        PublishInteger(
            address, "Blok", *state.blocked ? 1 : 0, previous.blocked, force);
    }
    PublishInteger(address, "Alarm", alarm, previous.alarm, force);
    PublishInteger(
        address, "AlarmCode", state.alarmCode, previous.alarmCode, force);
    PublishInteger(address, "Status", DeviceStatus(state), previous.status, force);
}

void MqttStatePublisher::Reset() noexcept
{
    published_ = {};
}

void MqttStatePublisher::PublishOffline(std::uint8_t address, bool force)
{
    auto& previous = published_[address];
    PublishInteger(address, "Alarm", 2, previous.alarm, force);
    PublishInteger(address, "Status", 7, previous.status, force);
}

void MqttStatePublisher::PublishInteger(
    std::uint8_t address,
    std::string_view control,
    int value,
    std::optional<int>& previous,
    bool force)
{
    if (!force && previous.has_value() && *previous == value) {
        return;
    }
    client_.Publish(Topic(address, control), std::to_string(value), true);
    previous = value;
}

void MqttStatePublisher::PublishNumber(
    std::uint8_t address,
    std::string_view control,
    double value,
    std::optional<double>& previous,
    bool force)
{
    if (!force && previous.has_value() && std::abs(*previous - value) < 0.0001) {
        return;
    }
    client_.Publish(Topic(address, control), FormatNumber(value), true);
    previous = value;
}

std::string MqttStatePublisher::Topic(
    std::uint8_t address,
    std::string_view control) const
{
    return "/devices/Fan-" + std::to_string(busNumber_) + "_" +
        std::to_string(address) + "/controls/" + std::string(control);
}

} // namespace mdv

namespace mdv {

MqttSystemPublisher::MqttSystemPublisher(
    int busNumber,
    IMqttClient& client,
    bool publishPollAddress)
    : busNumber_(busNumber),
      client_(client),
      publishPollAddress_(publishPollAddress)
{
    if (busNumber_ < 0) {
        throw std::invalid_argument("MQTT bus number cannot be negative");
    }
}

void MqttSystemPublisher::PublishSerial(std::string_view value, bool force)
{
    PublishText("Serial", value, serial_, force);
}

void MqttSystemPublisher::PublishError(std::string_view value, bool force)
{
    PublishText("Error", value, error_, force);
}

void MqttSystemPublisher::PublishAfter(const DriverResult& result)
{
    if (publishPollAddress_) {
        PublishInteger("GanGetID", static_cast<int>(result.address), pollAddress_, false);
    }

    switch (result.outcome) {
    case DriverOutcome::Success:
        PublishError("");
        break;
    case DriverOutcome::Timeout:
        // Per-device Alarm=2 and Status=7 already describe a normal timeout.
        // Do not overwrite the system Error topic on every missing fan coil.
        break;
    case DriverOutcome::IoError:
    case DriverOutcome::InvalidResponse:
        PublishError(result.error);
        break;
    }
}

void MqttSystemPublisher::Reset() noexcept
{
    serial_.reset();
    error_.reset();
    pollAddress_.reset();
}

void MqttSystemPublisher::PublishText(
    std::string_view control,
    std::string_view value,
    std::optional<std::string>& previous,
    bool force)
{
    if (!force && previous.has_value() && *previous == value) {
        return;
    }
    client_.Publish(Topic(control), value, true);
    previous = std::string(value);
}

void MqttSystemPublisher::PublishInteger(
    std::string_view control,
    int value,
    std::optional<int>& previous,
    bool force)
{
    if (!force && previous.has_value() && *previous == value) {
        return;
    }
    client_.Publish(Topic(control), std::to_string(value), true);
    previous = value;
}

std::string MqttSystemPublisher::Topic(std::string_view control) const
{
    return "/devices/sist-" + std::to_string(busNumber_) +
        "/controls/" + std::string(control);
}

} // namespace mdv
