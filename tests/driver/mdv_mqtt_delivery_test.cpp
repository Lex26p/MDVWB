#include "mdv_mosquitto.h"
#include <iostream>
#include <optional>
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

void TestFactualControlsComeOnlyFromC0()
{
    mdv::DeviceState state;
    state.command = mdv::Command::Read;
    state.mode = std::nullopt;
    state.fanSpeed = std::nullopt;
    state.setTemperature = 0;
    state.roomTemperature = std::nullopt;

    auto values = mdv::mqtt_detail::ConfirmedStateValuesFromC0(state);
    Require(!values.mode.has_value(),
            "missing C0 Mode became a factual MQTT value");
    Require(!values.speed.has_value(),
            "missing C0 Speed became a factual MQTT value");
    Require(!values.setTemperature.has_value(),
            "invalid C0 SetTemp became a factual MQTT value");
    Require(!values.roomTemperature.has_value(),
            "unavailable C0 T1 became a factual MQTT value");

    state.command = mdv::Command::Set;
    state.mode = mdv::Mode::Heat;
    state.fanSpeed = mdv::FanSpeed::High;
    state.setTemperature = 25;
    state.roomTemperature = 24.0;
    values = mdv::mqtt_detail::ConfirmedStateValuesFromC0(state);
    Require(!values.mode.has_value() && !values.speed.has_value() &&
                !values.setTemperature.has_value() &&
                !values.roomTemperature.has_value(),
            "C3 response values were accepted as factual MQTT state");

    state.command = mdv::Command::Read;
    values = mdv::mqtt_detail::ConfirmedStateValuesFromC0(state);
    Require(values.mode == mdv::Mode::Heat,
            "verified C0 Mode was not selected for MQTT");
    Require(values.speed == mdv::FanSpeed::High,
            "verified C0 Speed was not selected for MQTT");
    Require(values.setTemperature == 25,
            "verified C0 SetTemp was not selected for MQTT");
    Require(values.roomTemperature == 24.0,
            "verified C0 T1 was not selected for MQTT");
}

void TestUnavailableTemperatureClearsRetainedStateOnce()
{
    std::optional<double> previous;
    Require(mdv::mqtt_detail::ShouldPublishUnavailableNumber(previous, false),
            "first unavailable T1 would not clear a stale retained value");

    previous = mdv::mqtt_detail::UnavailableNumberMarker();
    Require(mdv::mqtt_detail::IsUnavailableNumberMarker(previous),
            "unavailable T1 marker was not recognized");
    Require(!mdv::mqtt_detail::ShouldPublishUnavailableNumber(previous, false),
            "unchanged unavailable T1 would publish an endless clear loop");
    Require(mdv::mqtt_detail::ShouldPublishUnavailableNumber(previous, true),
            "forced snapshot did not republish unavailable T1");

    previous = 23.5;
    Require(mdv::mqtt_detail::ShouldPublishUnavailableNumber(previous, false),
            "transition from numeric T1 to unavailable would keep stale Temp");
}

} // namespace

int main()
{
    try {
        TestDisconnectedTransportQueuesOnlyRetainedState();
        TestFactualControlsComeOnlyFromC0();
        TestUnavailableTemperatureClearsRetainedStateOnce();
        std::cout << "MDVWB MQTT delivery tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB MQTT delivery tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
