#include "mdv_modbus_profile_mqtt.h"

#include "mdv_mosquitto.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <thread>
#include <stdexcept>
#include <utility>

namespace mdvwb {
namespace {

[[nodiscard]] int ReadIntegerEnvironment(const char* name, int fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }

    try {
        std::size_t parsed = 0;
        const int result = std::stoi(value, &parsed);
        if (parsed != std::string_view(value).size()) {
            throw std::invalid_argument("trailing characters");
        }
        return result;
    }
    catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid integer in ") + name);
    }
}

[[nodiscard]] std::string ReadStringEnvironment(
    const char* name,
    std::string fallback = {})
{
    const char* value = std::getenv(name);
    return value == nullptr ? std::move(fallback) : std::string(value);
}

[[nodiscard]] mdv::MqttConnectionOptions PublisherOptionsFromEnvironment()
{
    mdv::MqttConnectionOptions options;
    options.host = ReadStringEnvironment("MDVWB_MQTT_HOST", "127.0.0.1");
    options.port = ReadIntegerEnvironment("MDVWB_MQTT_PORT", 1883);
    options.keepAliveSeconds =
        ReadIntegerEnvironment("MDVWB_MQTT_KEEPALIVE", 60);
    options.clientId = "mdvwb-manager-profile-catalog";
    options.username = ReadStringEnvironment("MDVWB_MQTT_USER");
    options.password = ReadStringEnvironment("MDVWB_MQTT_PASSWORD");
    options.reconnectDelaySeconds = static_cast<unsigned int>(
        ReadIntegerEnvironment("MDVWB_MQTT_RECONNECT", 1));
    options.reconnectDelayMaxSeconds = static_cast<unsigned int>(
        ReadIntegerEnvironment("MDVWB_MQTT_RECONNECT_MAX", 10));
    return options;
}

void SleepUntilRetry(const std::atomic_bool& stopRequested)
{
    constexpr auto RetryDelay = std::chrono::seconds(1);
    constexpr auto Slice = std::chrono::milliseconds(20);
    const auto deadline = std::chrono::steady_clock::now() + RetryDelay;
    while (!stopRequested.load() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(Slice);
    }
}

void RunPublisher(
    const std::filesystem::path& profileDirectory,
    const std::atomic_bool& stopRequested)
{
    if (!mdv::MosquittoMqttClient::IsSupported()) {
        return;
    }

    while (!stopRequested.load()) {
        try {
            mdv::MosquittoMqttClient client(
                PublisherOptionsFromEnvironment());
            const ModbusProfileUiPublishResult publication =
                PublishModbusProfileUiCatalog(client, profileDirectory);

            if (publication.status != mdv::MqttPublishStatus::Published &&
                publication.status != mdv::MqttPublishStatus::QueuedRetained) {
                SleepUntilRetry(stopRequested);
                continue;
            }

            client.Start();
            while (!stopRequested.load()) {
                if (client.IsConnected() &&
                    client.PendingPublicationCount() == 0U) {
                    client.Stop();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            client.Stop();
            return;
        }
        catch (const std::exception&) {
            SleepUntilRetry(stopRequested);
        }
    }
}

} // namespace

ModbusProfileUiRetainedPublisher::ModbusProfileUiRetainedPublisher(
    std::filesystem::path profileDirectory)
    : worker_(
          [this, directory = std::move(profileDirectory)] {
              RunPublisher(directory, stopRequested_);
          })
{
}

ModbusProfileUiRetainedPublisher::~ModbusProfileUiRetainedPublisher()
{
    stopRequested_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

} // namespace mdvwb
