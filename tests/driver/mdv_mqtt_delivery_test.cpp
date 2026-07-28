#include "mdv_mosquitto.h"
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void TestDisconnectedTransportQueuesOnlyRetainedState()
{
    mdv::MosquittoMqttClient client;

    Require(
        client.PublishWithResult("/test/state", "one", true) ==
            mdv::MqttPublishStatus::QueuedRetained,
        "disconnected retained publication did not report queued state");
    Require(client.PendingPublicationCount() == 1U,
            "retained state was not queued while disconnected");

    Require(
        client.PublishWithResult("/test/event", "run", false) ==
            mdv::MqttPublishStatus::Disconnected,
        "disconnected event did not report delivery failure");
    Require(client.PendingPublicationCount() == 1U,
            "non-retained event entered the reconnect queue");
    Require(!client.LastError().empty(),
            "dropped non-retained event did not expose a diagnostic");

    Require(
        client.PublishWithResult("/test/state", "two", true) ==
            mdv::MqttPublishStatus::QueuedRetained,
        "updated retained publication did not remain queued");
    Require(client.PendingPublicationCount() == 1U,
            "retained state was not coalesced by topic");

    Require(
        client.PublishWithResult("/test/other-state", "value", true) ==
            mdv::MqttPublishStatus::QueuedRetained,
        "second retained publication did not report queued state");
    Require(client.PendingPublicationCount() == 2U,
            "independent retained state was not queued");
}

} // namespace

int main()
{
    try {
        TestDisconnectedTransportQueuesOnlyRetainedState();
        std::cout << "MDVWB MQTT delivery tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB MQTT delivery tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
