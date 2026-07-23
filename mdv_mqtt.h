#pragma once

#include "mdv_driver.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
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

} // namespace mdv
