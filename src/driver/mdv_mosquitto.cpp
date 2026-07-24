#include "mdv_mosquitto.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

#if defined(MDVWB_HAS_MOSQUITTO)
#include <mosquitto.h>
#endif

namespace mdv {
namespace {

void ValidateOptions(const MqttConnectionOptions& options)
{
    if (options.host.empty()) {
        throw std::invalid_argument("MQTT host cannot be empty");
    }
    if (options.port <= 0 || options.port > 65535) {
        throw std::invalid_argument("MQTT port must be in range 1..65535");
    }
    if (options.keepAliveSeconds <= 0) {
        throw std::invalid_argument("MQTT keepalive must be positive");
    }
    if (options.clientId.empty()) {
        throw std::invalid_argument("MQTT client ID cannot be empty");
    }
    if (options.reconnectDelaySeconds == 0 ||
        options.reconnectDelayMaxSeconds < options.reconnectDelaySeconds) {
        throw std::invalid_argument("invalid MQTT reconnect delay range");
    }
    if (options.username.empty() && !options.password.empty()) {
        throw std::invalid_argument("MQTT password requires a username");
    }
}

#if defined(MDVWB_HAS_MOSQUITTO)

class MosquittoLibrary final {
public:
    MosquittoLibrary()
    {
        const auto result = mosquitto_lib_init();
        if (result != MOSQ_ERR_SUCCESS) {
            throw std::runtime_error(
                std::string("mosquitto_lib_init failed: ") +
                mosquitto_strerror(result));
        }
    }

    ~MosquittoLibrary()
    {
        mosquitto_lib_cleanup();
    }
};

MosquittoLibrary& Library()
{
    static MosquittoLibrary library;
    return library;
}

#endif

} // namespace

struct MosquittoMqttClient::Implementation {
#if defined(MDVWB_HAS_MOSQUITTO)
    mosquitto* client = nullptr;
#endif
};

MosquittoMqttClient::MosquittoMqttClient(MqttConnectionOptions options)
    : options_(std::move(options)), implementation_(std::make_unique<Implementation>())
{
    ValidateOptions(options_);
}

MosquittoMqttClient::~MosquittoMqttClient()
{
    Stop();
}

void MosquittoMqttClient::Start()
{
    {
        std::lock_guard lock(mutex_);
        if (started_) {
            return;
        }
    }

#if defined(MDVWB_HAS_MOSQUITTO)
    (void)Library();

    auto* client = mosquitto_new(options_.clientId.c_str(), true, this);
    if (client == nullptr) {
        throw std::runtime_error("mosquitto_new failed");
    }

    mosquitto_connect_callback_set(
        client,
        [](mosquitto*, void* userData, int resultCode) {
            static_cast<MosquittoMqttClient*>(userData)->HandleConnected(resultCode);
        });
    mosquitto_disconnect_callback_set(
        client,
        [](mosquitto*, void* userData, int resultCode) {
            static_cast<MosquittoMqttClient*>(userData)->HandleDisconnected(resultCode);
        });
    mosquitto_message_callback_set(
        client,
        [](mosquitto*, void* userData, const mosquitto_message* message) {
            if (message == nullptr) {
                return;
            }
            static_cast<MosquittoMqttClient*>(userData)->HandleMessage(
                message->topic,
                message->payload,
                message->payloadlen,
                message->retain);
        });

    if (!options_.username.empty()) {
        const auto result = mosquitto_username_pw_set(
            client,
            options_.username.c_str(),
            options_.password.empty() ? nullptr : options_.password.c_str());
        if (result != MOSQ_ERR_SUCCESS) {
            mosquitto_destroy(client);
            throw std::runtime_error(
                std::string("mosquitto_username_pw_set failed: ") +
                mosquitto_strerror(result));
        }
    }

    auto result = mosquitto_reconnect_delay_set(
        client,
        options_.reconnectDelaySeconds,
        options_.reconnectDelayMaxSeconds,
        true);
    if (result != MOSQ_ERR_SUCCESS) {
        mosquitto_destroy(client);
        throw std::runtime_error(
            std::string("mosquitto_reconnect_delay_set failed: ") +
            mosquitto_strerror(result));
    }

    {
        std::lock_guard lock(mutex_);
        implementation_->client = client;
        started_ = true;
        connected_ = false;
        lastError_.clear();
    }

    result = mosquitto_connect_async(
        client,
        options_.host.c_str(),
        options_.port,
        options_.keepAliveSeconds);
    if (result != MOSQ_ERR_SUCCESS) {
        {
            std::lock_guard lock(mutex_);
            implementation_->client = nullptr;
            started_ = false;
        }
        mosquitto_destroy(client);
        throw std::runtime_error(
            std::string("mosquitto_connect_async failed: ") +
            mosquitto_strerror(result));
    }

    result = mosquitto_loop_start(client);
    if (result != MOSQ_ERR_SUCCESS) {
        {
            std::lock_guard lock(mutex_);
            implementation_->client = nullptr;
            started_ = false;
        }
        mosquitto_destroy(client);
        throw std::runtime_error(
            std::string("mosquitto_loop_start failed: ") +
            mosquitto_strerror(result));
    }
#else
    throw std::runtime_error(
        "libmosquitto support is not available in this build");
#endif
}

void MosquittoMqttClient::Stop() noexcept
{
#if defined(MDVWB_HAS_MOSQUITTO)
    mosquitto* client = nullptr;
    {
        std::lock_guard lock(mutex_);
        if (!started_) {
            return;
        }
        started_ = false;
        connected_ = false;
        client = implementation_->client;
        implementation_->client = nullptr;
    }

    if (client != nullptr) {
        mosquitto_disconnect(client);
        mosquitto_loop_stop(client, true);
        mosquitto_destroy(client);
    }
#else
    std::lock_guard lock(mutex_);
    started_ = false;
    connected_ = false;
#endif
}

bool MosquittoMqttClient::IsSupported() noexcept
{
#if defined(MDVWB_HAS_MOSQUITTO)
    return true;
#else
    return false;
#endif
}

bool MosquittoMqttClient::IsConnected() const noexcept
{
    std::lock_guard lock(mutex_);
    return connected_;
}

std::string MosquittoMqttClient::LastError() const
{
    std::lock_guard lock(mutex_);
    return lastError_;
}

std::size_t MosquittoMqttClient::SubscriptionCount() const
{
    std::lock_guard lock(mutex_);
    return subscriptions_.size();
}

std::size_t MosquittoMqttClient::PendingPublicationCount() const
{
    std::lock_guard lock(mutex_);
    return pendingPublications_.size();
}

void MosquittoMqttClient::SetMessageHandler(MessageHandler handler)
{
    std::lock_guard lock(mutex_);
    messageHandler_ = std::move(handler);
}

void MosquittoMqttClient::Subscribe(std::string_view topicFilter)
{
    if (topicFilter.empty()) {
        throw std::invalid_argument("MQTT subscription topic cannot be empty");
    }

    std::string topic(topicFilter);
#if defined(MDVWB_HAS_MOSQUITTO)
    mosquitto* client = nullptr;
#endif
    bool connected = false;
    {
        std::lock_guard lock(mutex_);
        if (std::find(subscriptions_.begin(), subscriptions_.end(), topic) ==
            subscriptions_.end()) {
            subscriptions_.push_back(topic);
        }
        connected = connected_;
#if defined(MDVWB_HAS_MOSQUITTO)
        client = implementation_->client;
#endif
    }

#if defined(MDVWB_HAS_MOSQUITTO)
    if (connected && client != nullptr) {
        const auto result = mosquitto_subscribe(client, nullptr, topic.c_str(), 0);
        if (result != MOSQ_ERR_SUCCESS) {
            SetError(
                std::string("mosquitto_subscribe failed: ") +
                mosquitto_strerror(result));
        }
    }
#else
    (void)connected;
#endif
}

void MosquittoMqttClient::Publish(
    std::string_view topic,
    std::string_view payload,
    bool retained)
{
    if (topic.empty()) {
        throw std::invalid_argument("MQTT publication topic cannot be empty");
    }

    MqttPublication publication{
        .topic = std::string(topic),
        .payload = std::string(payload),
        .retained = retained,
    };

#if defined(MDVWB_HAS_MOSQUITTO)
    mosquitto* client = nullptr;
#endif
    bool connected = false;
    {
        std::lock_guard lock(mutex_);
        connected = connected_;
#if defined(MDVWB_HAS_MOSQUITTO)
        client = implementation_->client;
#endif
        if (!connected) {
            pendingPublications_[publication.topic] = std::move(publication);
            return;
        }
    }

#if defined(MDVWB_HAS_MOSQUITTO)
    const auto result = mosquitto_publish(
        client,
        nullptr,
        publication.topic.c_str(),
        static_cast<int>(publication.payload.size()),
        publication.payload.data(),
        0,
        publication.retained);
    if (result != MOSQ_ERR_SUCCESS) {
        {
            std::lock_guard lock(mutex_);
            pendingPublications_[publication.topic] = publication;
        }
        SetError(
            std::string("mosquitto_publish failed: ") +
            mosquitto_strerror(result));
    }
#else
    (void)connected;
#endif
}

void MosquittoMqttClient::SetError(std::string error)
{
    std::lock_guard lock(mutex_);
    lastError_ = std::move(error);
}

void MosquittoMqttClient::HandleConnected(int resultCode) noexcept
{
#if defined(MDVWB_HAS_MOSQUITTO)
    if (resultCode != 0) {
        SetError(
            std::string("MQTT broker rejected connection: ") +
            mosquitto_connack_string(resultCode));
        return;
    }
#else
    (void)resultCode;
#endif

    {
        std::lock_guard lock(mutex_);
        connected_ = true;
        lastError_.clear();
    }
    FlushAfterConnect();
}

void MosquittoMqttClient::HandleDisconnected(int resultCode) noexcept
{
    std::lock_guard lock(mutex_);
    connected_ = false;
    if (started_ && resultCode != 0) {
        lastError_ = "MQTT connection lost with code " + std::to_string(resultCode);
    }
}

void MosquittoMqttClient::HandleMessage(
    const char* topic,
    const void* payload,
    int payloadLength,
    bool retained) noexcept
{
    if (topic == nullptr || payloadLength < 0 ||
        (payloadLength > 0 && payload == nullptr)) {
        return;
    }

    MessageHandler handler;
    {
        std::lock_guard lock(mutex_);
        handler = messageHandler_;
    }
    if (!handler) {
        return;
    }

    try {
        MqttMessage message;
        message.topic = topic;
        if (payloadLength > 0) {
            const auto* data = static_cast<const char*>(payload);
            message.payload.assign(data, data + payloadLength);
        }
        message.retained = retained;
        handler(std::move(message));
    }
    catch (...) {
        // Never allow exceptions to cross the C callback boundary.
    }
}

void MosquittoMqttClient::FlushAfterConnect() noexcept
{
#if defined(MDVWB_HAS_MOSQUITTO)
    mosquitto* client = nullptr;
    std::vector<std::string> subscriptions;
    std::vector<MqttPublication> publications;
    {
        std::lock_guard lock(mutex_);
        client = implementation_->client;
        subscriptions = subscriptions_;
        publications.reserve(pendingPublications_.size());
        for (const auto& [topic, publication] : pendingPublications_) {
            (void)topic;
            publications.push_back(publication);
        }
        pendingPublications_.clear();
    }

    if (client == nullptr) {
        return;
    }

    for (const auto& topic : subscriptions) {
        const auto result = mosquitto_subscribe(client, nullptr, topic.c_str(), 0);
        if (result != MOSQ_ERR_SUCCESS) {
            SetError(
                std::string("mosquitto_subscribe failed: ") +
                mosquitto_strerror(result));
        }
    }

    for (const auto& publication : publications) {
        const auto result = mosquitto_publish(
            client,
            nullptr,
            publication.topic.c_str(),
            static_cast<int>(publication.payload.size()),
            publication.payload.data(),
            0,
            publication.retained);
        if (result != MOSQ_ERR_SUCCESS) {
            std::lock_guard lock(mutex_);
            pendingPublications_[publication.topic] = publication;
            lastError_ = std::string("mosquitto_publish failed: ") +
                mosquitto_strerror(result);
        }
    }
#endif
}

} // namespace mdv
