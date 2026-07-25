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

    client.Publish("/test/state", "one", true);
    Require(client.PendingPublicationCount() == 1U,
            "retained state was not queued while disconnected");

    client.Publish("/test/event", "run", false);
    Require(client.PendingPublicationCount() == 1U,
            "non-retained event entered the reconnect queue");
    Require(!client.LastError().empty(),
            "dropped non-retained event did not expose a diagnostic");

    client.Publish("/test/state", "two", true);
    Require(client.PendingPublicationCount() == 1U,
            "retained state was not coalesced by topic");

    client.Publish("/test/other-state", "value", true);
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
