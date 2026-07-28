#pragma once

#include "mdv_driver.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace mdv {

struct MqttMessage {
    std::string topic;
    std::string payload;
    bool retained = false;
};

struct MqttPublication {
    std::string topic;
    std::string payload;
    bool retained = false;
};

namespace mqtt_detail {

// Values eligible for factual MQTT publication. The function deliberately
// accepts only C0 data and never consults the cached C3 command frame.
struct ConfirmedStateValues {
    std::optional<Mode> mode;
    std::optional<FanSpeed> speed;
    std::optional<std::uint8_t> setTemperature;
    std::optional<double> roomTemperature;
};

[[nodiscard]] inline ConfirmedStateValues ConfirmedStateValuesFromC0(
    const DeviceState& state) noexcept
{
    if (state.command != Command::Read) {
        return {};
    }

    ConfirmedStateValues values;
    values.mode = state.mode;
    values.speed = state.fanSpeed;
    if (state.setTemperature >= 16 && state.setTemperature <= 32) {
        values.setTemperature = state.setTemperature;
    }
    values.roomTemperature = state.roomTemperature;
    return values;
}

// NaN is used only inside PublishedState to distinguish an already-cleared
// retained numeric topic from a value that has never been processed.
[[nodiscard]] inline double UnavailableNumberMarker() noexcept
{
    return std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] inline bool IsUnavailableNumberMarker(
    const std::optional<double>& value) noexcept
{
    return value.has_value() && std::isnan(*value);
}

[[nodiscard]] inline bool ShouldPublishUnavailableNumber(
    const std::optional<double>& previous,
    bool force) noexcept
{
    return force || !IsUnavailableNumberMarker(previous);
}

} // namespace mqtt_detail

enum class MqttPublishStatus {
    Published,
    QueuedRetained,
    Disconnected,
    Failed,
};

[[nodiscard]] constexpr std::string_view MqttPublishStatusMessage(
    MqttPublishStatus status) noexcept {
    switch (status) {
        case MqttPublishStatus::Published:
            return "published";
        case MqttPublishStatus::QueuedRetained:
            return "retained publication queued for reconnect";
        case MqttPublishStatus::Disconnected:
            return "MQTT client is disconnected";
        case MqttPublishStatus::Failed:
            return "MQTT publication failed";
    }
    return "unknown MQTT publication status";
}

// The network callback only pushes messages here. RS-485 state is changed later
// by the driver thread, so MQTT and serial code never access DeviceContext at
// the same time.
class MqttCommandInbox {
public:
    void Push(MqttMessage message);
    [[nodiscard]] std::optional<MqttMessage> TryPop();
    [[nodiscard]] std::size_t Size() const;

private:
    mutable std::mutex mutex_;
    std::deque<MqttMessage> messages_;
};

class IMqttClient {
public:
    using MessageHandler = std::function<void(MqttMessage)>;

    virtual ~IMqttClient() = default;
    virtual void SetMessageHandler(MessageHandler handler) = 0;
    virtual void Subscribe(std::string_view topicFilter) = 0;
    virtual void Publish(
        std::string_view topic,
        std::string_view payload,
        bool retained) = 0;

    // Existing in-memory clients remain source-compatible: their Publish()
    // implementation is treated as an immediate successful publication.
    // Network transports override this method to expose disconnects and
    // synchronous library failures to command-producing services.
    [[nodiscard]] virtual MqttPublishStatus PublishWithResult(
        std::string_view topic,
        std::string_view payload,
        bool retained) {
        Publish(topic, payload, retained);
        return MqttPublishStatus::Published;
    }
};

enum class MqttCommandStatus {
    Applied,
    Ignored,
    InvalidTopic,
    InvalidPayload,
    DeviceNotConfigured,
    DeviceNotInitialized,
};

struct MqttCommandResult {
    MqttCommandStatus status = MqttCommandStatus::Ignored;
    std::optional<std::uint8_t> address;
    std::string control;
    std::string error;
};

class MqttCommandRouter {
public:
    MqttCommandRouter(int busNumber, MdvDriver& driver);

    [[nodiscard]] static std::string_view SubscriptionTopic() noexcept;
    [[nodiscard]] MqttCommandResult Handle(const MqttMessage& message);

private:
    [[nodiscard]] MqttCommandResult Apply(
        std::uint8_t address,
        std::string_view control,
        int value);

    int busNumber_ = 0;
    MdvDriver& driver_;
};

class MqttCommandService {
public:
    MqttCommandService(IMqttClient& client, MqttCommandRouter& router);

    void Start();
    [[nodiscard]] std::optional<MqttCommandResult> ProcessOne();
    [[nodiscard]] std::size_t PendingCount() const;

private:
    IMqttClient& client_;
    MqttCommandRouter& router_;
    MqttCommandInbox inbox_;
    bool started_ = false;
};

// Publishes confirmed fan-coil values to the main Wiren Board control topics
// with MQTT retain enabled. One previous value is stored per control, therefore
// an unchanged C0 response produces no MQTT traffic. C3/CC/CD replies are never
// published because they may contain stale data; only a verified C0 updates
// retained state topics.
class MqttStatePublisher {
public:
    MqttStatePublisher(int busNumber, IMqttClient& client);

    void PublishAfter(const MdvDriver& driver, const DriverResult& result);
    void PublishDevice(const DeviceRuntime& runtime, bool force = false);
    void Reset() noexcept;

private:
    struct PublishedState {
        std::optional<int> power;
        std::optional<int> mode;
        std::optional<int> speed;
        std::optional<int> setTemperature;
        std::optional<double> roomTemperature;
        std::optional<int> blinds;
        std::optional<int> blocked;
        std::optional<int> alarm;
        std::optional<int> alarmCode;
        std::optional<int> status;
    };

    void PublishOffline(std::uint8_t address, bool force);
    void PublishInteger(
        std::uint8_t address,
        std::string_view control,
        int value,
        std::optional<int>& previous,
        bool force);
    void PublishNumber(
        std::uint8_t address,
        std::string_view control,
        double value,
        std::optional<double>& previous,
        bool force);
    [[nodiscard]] std::string Topic(
        std::uint8_t address,
        std::string_view control) const;

    int busNumber_ = 0;
    IMqttClient& client_;
    std::array<PublishedState, kMaxDeviceAddress + 1> published_{};
};

// Publishes the separate system device used by the existing Wiren Board
// script. Serial and Error use the original base topics without /on.
// GanGetID is optional because publishing every 150 ms creates unnecessary
// MQTT traffic during normal operation.
class MqttSystemPublisher {
public:
    MqttSystemPublisher(
        int busNumber,
        IMqttClient& client,
        bool publishPollAddress = false);

    void PublishSerial(std::string_view value, bool force = false);
    void PublishError(std::string_view value, bool force = false);
    void PublishAfter(const DriverResult& result);
    void Reset() noexcept;

private:
    void PublishText(
        std::string_view control,
        std::string_view value,
        std::optional<std::string>& previous,
        bool force);
    void PublishInteger(
        std::string_view control,
        int value,
        std::optional<int>& previous,
        bool force);
    [[nodiscard]] std::string Topic(std::string_view control) const;

    int busNumber_ = 0;
    IMqttClient& client_;
    bool publishPollAddress_ = false;
    std::optional<std::string> serial_;
    std::optional<std::string> error_;
    std::optional<int> pollAddress_;
};

} // namespace mdv
