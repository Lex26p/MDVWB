#include "mdv_driver.h"
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

void TestSemanticStateComesOnlyFromConfirmedRead()
{
    mdv::DeviceRuntime incomplete(1);
    mdv::DeviceState incompleteState;
    incompleteState.command = mdv::Command::Read;
    incompleteState.address = 1;
    incompleteState.power = false;
    incompleteState.mode = std::nullopt;
    incompleteState.fanSpeed = std::nullopt;
    incompleteState.setTemperature = 0;
    incompleteState.roomTemperature = std::nullopt;

    incomplete.device.SynchronizeReadState(incompleteState);
    incomplete.online = true;

    const auto incompleteSemantic = incomplete.SemanticState();
    Require(incompleteSemantic.hasState,
            "verified C0 state was not exposed through the driver boundary");
    Require(!incompleteSemantic.mode.has_value(),
            "missing C0 Mode became a factual semantic value");
    Require(!incompleteSemantic.fanSpeed.has_value(),
            "missing C0 Speed became a factual semantic value");
    Require(!incompleteSemantic.setTemperature.has_value(),
            "invalid C0 SetTemp became a factual semantic value");
    Require(!incompleteSemantic.roomTemperature.has_value(),
            "unavailable C0 T1 became a factual semantic value");

    mdv::DeviceRuntime runtime(2);
    mdv::DeviceState state;
    state.command = mdv::Command::Read;
    state.address = 2;
    state.power = true;
    state.mode = mdv::Mode::Heat;
    state.activeMode = mdv::Mode::Heat;
    state.fanSpeed = mdv::FanSpeed::High;
    state.activeFanSpeed = mdv::FanSpeed::High;
    state.setTemperature = 25;
    state.roomTemperature = 24.0;

    runtime.device.SynchronizeReadState(state);
    runtime.online = true;

    auto semantic = runtime.SemanticState();
    Require(semantic.mode == mdv::HvacMode::Heat,
            "verified C0 Mode was not mapped to semantic state");
    Require(semantic.fanSpeed == mdv::HvacFanSpeed::High,
            "verified C0 Speed was not mapped to semantic state");
    Require(semantic.setTemperature == 25.0,
            "verified C0 SetTemp was not mapped to semantic state");
    Require(semantic.roomTemperature == 24.0,
            "verified C0 T1 was not mapped to semantic state");

    state.command = mdv::Command::Set;
    state.mode = mdv::Mode::Cool;
    state.fanSpeed = mdv::FanSpeed::Low;
    state.setTemperature = 19;
    state.roomTemperature = 18.5;

    bool c3Rejected = false;
    try {
        runtime.device.SynchronizeReadState(state);
    }
    catch (const std::invalid_argument&) {
        c3Rejected = true;
    }
    Require(c3Rejected,
            "non-C0 state was accepted as confirmed device state");

    semantic = runtime.SemanticState();
    Require(semantic.mode == mdv::HvacMode::Heat &&
                semantic.fanSpeed == mdv::HvacFanSpeed::High &&
                semantic.setTemperature == 25.0 &&
                semantic.roomTemperature == 24.0,
            "rejected non-C0 state changed factual semantic values");
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
        TestSemanticStateComesOnlyFromConfirmedRead();
        TestUnavailableTemperatureClearsRetainedStateOnce();
        std::cout << "MDVWB MQTT delivery tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB MQTT delivery tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
