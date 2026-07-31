#pragma once

#include "device_driver.h"
#include "mdv_bounded_queue.h"

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

[[nodiscard]] inline bool SameQueueKey(
    const MqttMessage& left,
    const MqttMessage& right) noexcept
{
    return left.topic == right.topic;
}

[[nodiscard]] inline std::size_t QueueItemBytes(
    const MqttMessage& message) noexcept
{
    return sizeof(MqttMessage) + message.topic.size() + message.payload.size();
}

struct MqttPublication {
    std::string topic;
    std::string payload;
    bool retained = false;
};

namespace mqtt_detail {

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

// The network callback only pushes messages here. Driver state is changed later
// by the driver thread, so MQTT and serial code never mutate the same device at
// the same time.
class MqttCommandInbox {
public:
    static constexpr std::size_t MaximumPendingMessages = 512U;
    static constexpr std::size_t MaximumPendingBytes = 256U * 1024U;

    void Push(MqttMessage message);
    [[nodiscard]] std::optional<MqttMessage> TryPop();
    [[nodiscard]] std::size_t Size() const;

private:
    mutable std::mutex mutex_;
    BoundedLatestQueue<
        MqttMessage, MaximumPendingMessages, MaximumPendingBytes> messages_;
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
    MqttCommandRouter(int busNumber, IDeviceDriver& driver);

    [[nodiscard]] static std::string_view SubscriptionTopic() noexcept;
    [[nodiscard]] MqttCommandResult Handle(const MqttMessage& message);

private:
    [[nodiscard]] MqttCommandResult Apply(
        std::uint8_t address,
        std::string_view control,
        int value);

    int busNumber_ = 0;
    IDeviceDriver& driver_;
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

// Publishes confirmed semantic driver values to the existing Wiren Board MQTT
// controls. Protocol-specific wire frames are deliberately invisible here.
class MqttStatePublisher {
public:
    MqttStatePublisher(int busNumber, IMqttClient& client);

    void PublishAfter(const IDeviceDriver& driver, const DriverResult& result);
    void PublishDevice(const DriverDeviceState& state, bool force = false);
    void Reset() noexcept;

private:
    struct PublishedState {
        std::optional<int> power;
        std::optional<int> mode;
        std::optional<int> speed;
        std::optional<double> setTemperature;
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
    std::array<PublishedState, kMaxLogicalDeviceAddress + 1> published_{};
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
