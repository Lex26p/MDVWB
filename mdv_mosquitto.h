#pragma once

#include "mdv_mqtt.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace mdv {

struct MqttConnectionOptions {
    std::string host = "127.0.0.1";
    int port = 1883;
    int keepAliveSeconds = 60;
    std::string clientId = "mdvwb";
    std::string username;
    std::string password;
    unsigned int reconnectDelaySeconds = 1;
    unsigned int reconnectDelayMaxSeconds = 10;
};

// Real MQTT transport backed by libmosquitto. CMake enables the network part
// automatically when both mosquitto.h and the mosquitto library are available.
// Before the connection is ready, subscriptions are remembered and only the
// latest publication for every topic is retained in memory. They are flushed
// after a successful reconnect.
class MosquittoMqttClient final : public IMqttClient {
public:
    explicit MosquittoMqttClient(MqttConnectionOptions options = {});
    ~MosquittoMqttClient() override;

    MosquittoMqttClient(const MosquittoMqttClient&) = delete;
    MosquittoMqttClient& operator=(const MosquittoMqttClient&) = delete;

    void Start();
    void Stop() noexcept;

    [[nodiscard]] static bool IsSupported() noexcept;
    [[nodiscard]] bool IsConnected() const noexcept;
    [[nodiscard]] std::string LastError() const;
    [[nodiscard]] std::size_t SubscriptionCount() const;
    [[nodiscard]] std::size_t PendingPublicationCount() const;

    void SetMessageHandler(MessageHandler handler) override;
    void Subscribe(std::string_view topicFilter) override;
    void Publish(
        std::string_view topic,
        std::string_view payload,
        bool retained) override;

private:
    struct Implementation;

    void SetError(std::string error);
    void HandleConnected(int resultCode) noexcept;
    void HandleDisconnected(int resultCode) noexcept;
    void HandleMessage(
        const char* topic,
        const void* payload,
        int payloadLength,
        bool retained) noexcept;
    void FlushAfterConnect() noexcept;

    MqttConnectionOptions options_;
    mutable std::mutex mutex_;
    MessageHandler messageHandler_;
    std::vector<std::string> subscriptions_;
    std::unordered_map<std::string, MqttPublication> pendingPublications_;
    std::string lastError_;
    bool started_ = false;
    bool connected_ = false;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace mdv
