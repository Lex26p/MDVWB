#include "mdv_mqtt.h"
#include "modbus_driver.h"
#include "modbus_profile.h"
#include "modbus_rtu.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef MDVWB_SOURCE_DIR
#error MDVWB_SOURCE_DIR must point to the repository source directory
#endif

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

[[nodiscard]] std::uint16_t RequestAddress(
    const mdv::modbus::RtuAdu& request)
{
    Require(request.size() >= 6U, "Modbus request is too short");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(request[2]) << 8U) |
        static_cast<std::uint16_t>(request[3]));
}

[[nodiscard]] mdv::modbus::TransactionResult ReadSuccess(
    const mdv::modbus::RtuAdu& request,
    std::uint16_t value)
{
    mdv::modbus::ParsedResponse response;
    response.status = mdv::modbus::ResponseStatus::Success;
    response.slaveId = request[0];
    response.function = mdv::modbus::Function::ReadHoldingRegisters;
    response.registers = {value};

    mdv::modbus::TransactionResult result;
    result.status = mdv::modbus::TransactionStatus::Success;
    result.response = std::move(response);
    return result;
}

[[nodiscard]] mdv::modbus::TransactionResult WriteSuccess(
    const mdv::modbus::RtuAdu& request)
{
    mdv::modbus::ParsedResponse response;
    response.status = mdv::modbus::ResponseStatus::Success;
    response.slaveId = request[0];
    response.function = mdv::modbus::Function::WriteMultipleRegisters;
    response.startAddress = RequestAddress(request);
    response.quantity = 1U;

    mdv::modbus::TransactionResult result;
    result.status = mdv::modbus::TransactionStatus::Success;
    result.response = std::move(response);
    return result;
}

class RuntimeTransport final
    : public mdv::modbus::ITransactionTransport {
public:
    [[nodiscard]] mdv::modbus::TransactionResult Execute(
        const mdv::modbus::RtuAdu& request) override
    {
        requests.push_back(request);
        const auto function = request.at(1);
        const auto address = RequestAddress(request);

        if (function == static_cast<std::uint8_t>(
                mdv::modbus::Function::WriteMultipleRegisters)) {
            Require(address == 40078U, "Power FC10 address mismatch");
            Require(request.size() == 11U, "Power FC10 request size mismatch");
            Require(request[4] == 0U && request[5] == 1U,
                    "Power FC10 quantity mismatch");
            Require(request[6] == 2U && request[7] == 0U && request[8] == 1U,
                    "Power ON was not encoded as raw 1");
            ++writeCount;
            return WriteSuccess(request);
        }

        Require(
            function == static_cast<std::uint8_t>(
                mdv::modbus::Function::ReadHoldingRegisters),
            "unexpected Modbus function in MQTT integration test");

        if (address == 40039U) {
            ++probeCount;
            // Initial snapshot sees the device. The next ordinary poll after
            // the confirmed command reports absence and must publish offline.
            return ReadSuccess(request, probeCount == 1U ? 24U : 0U);
        }
        if (address == 40028U) {
            ++powerReadCount;
            // Initial factual state is OFF; confirmation after FC10 is ON.
            return ReadSuccess(request, powerReadCount == 1U ? 0U : 1U);
        }
        if (address == 40035U) {
            return ReadSuccess(request, 0U);
        }

        throw std::runtime_error(
            "unexpected Modbus register " + std::to_string(address));
    }

    std::vector<mdv::modbus::RtuAdu> requests;
    std::size_t probeCount = 0;
    std::size_t powerReadCount = 0;
    std::size_t writeCount = 0;
};

class FakeMqttClient final : public mdv::IMqttClient {
public:
    void SetMessageHandler(MessageHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void Subscribe(std::string_view topicFilter) override
    {
        subscriptions.emplace_back(topicFilter);
    }

    void Publish(
        std::string_view topic,
        std::string_view payload,
        bool retained) override
    {
        publications.push_back(mdv::MqttPublication{
            .topic = std::string(topic),
            .payload = std::string(payload),
            .retained = retained,
        });
    }

    void Emit(
        std::string topic,
        std::string payload,
        bool retained = false)
    {
        if (!handler_) {
            throw std::logic_error("fake MQTT client has no handler");
        }
        handler_(mdv::MqttMessage{
            .topic = std::move(topic),
            .payload = std::move(payload),
            .retained = retained,
        });
    }

    [[nodiscard]] bool HasPublication(
        std::string_view topic,
        std::string_view payload,
        std::size_t begin = 0U) const
    {
        return std::any_of(
            publications.begin() + static_cast<std::ptrdiff_t>(begin),
            publications.end(),
            [topic, payload](const mdv::MqttPublication& publication) {
                return publication.topic == topic &&
                    publication.payload == payload &&
                    publication.retained;
            });
    }

    std::vector<std::string> subscriptions;
    std::vector<mdv::MqttPublication> publications;

private:
    MessageHandler handler_;
};

[[nodiscard]] mdv::modbus::ModbusProfile ProductionProfile()
{
    return mdv::modbus::LoadProfileFile(
        std::filesystem::path(MDVWB_SOURCE_DIR) /
        "profiles/modbus/vrf_add_controller.json");
}

void TestModbusStateAndPowerCommandUseExistingMqttBoundary()
{
    auto profile = ProductionProfile();
    RuntimeTransport transport;
    mdv::modbus::ModbusDriver driver({1U}, profile, transport);
    FakeMqttClient mqtt;
    mdv::MqttCommandRouter router(2, driver);
    mdv::MqttCommandService commands(mqtt, router);
    mdv::MqttStatePublisher states(2, mqtt);

    commands.Start();
    Require(
        mqtt.subscriptions ==
            std::vector<std::string>{"/devices/+/controls/+/on1"},
        "Modbus runtime changed the MQTT command subscription");

    const auto initial = driver.ProcessNext();
    states.PublishAfter(driver, initial);
    Require(
        initial.outcome == mdv::DriverOutcome::Success,
        "initial Modbus factual snapshot failed");
    Require(
        mqtt.HasPublication(
            "/devices/Fan-2_1/controls/Power", "0"),
        "initial Modbus Power was not published on the existing topic");
    Require(
        mqtt.HasPublication(
            "/devices/Fan-2_1/controls/Alarm", "0"),
        "initial Modbus Alarm was not published on the existing topic");
    Require(
        mqtt.HasPublication(
            "/devices/Fan-2_1/controls/AlarmCode", "0"),
        "initial Modbus AlarmCode was not published on the existing topic");

    const auto requestsBeforeCommand = transport.requests.size();
    mqtt.Emit(
        "/devices/Fan-2_1/controls/Power/on1",
        "1");
    Require(commands.PendingCount() == 1U, "MQTT command was not queued");
    Require(
        transport.requests.size() == requestsBeforeCommand,
        "MQTT callback generated Modbus traffic directly");

    const auto routed = commands.ProcessOne();
    Require(routed.has_value(), "queued MQTT command was lost");
    Require(
        routed->status == mdv::MqttCommandStatus::Applied,
        "Power MQTT command was not accepted by ModbusDriver");
    Require(
        transport.requests.size() == requestsBeforeCommand,
        "command routing generated Modbus traffic outside driver thread");

    const auto publicationsBeforeWrite = mqtt.publications.size();
    const auto write = driver.ProcessNext();
    states.PublishAfter(driver, write);
    Require(
        write.operation == mdv::DriverOperation::SetState &&
            write.outcome == mdv::DriverOutcome::Success,
        "Power FC10 operation failed");
    Require(transport.writeCount == 1U, "Power FC10 was not sent exactly once");
    Require(
        mqtt.publications.size() == publicationsBeforeWrite,
        "FC10 acknowledgement was published as factual Power");
    Require(
        !driver.DeviceStateByAddress(1U).power,
        "FC10 acknowledgement changed factual Power before read-back");

    const auto confirmation = driver.ProcessNext();
    states.PublishAfter(driver, confirmation);
    Require(
        confirmation.operation == mdv::DriverOperation::ConfirmRead &&
            confirmation.outcome == mdv::DriverOutcome::Success,
        "Power confirmation read failed");
    Require(
        driver.DeviceStateByAddress(1U).power,
        "confirmed Power was not stored in semantic state");
    Require(
        mqtt.HasPublication(
            "/devices/Fan-2_1/controls/Power",
            "1",
            publicationsBeforeWrite),
        "confirmed Power did not publish on the existing retained topic");

    const auto trafficBeforeUnsupported = transport.requests.size();
    mqtt.Emit(
        "/devices/Fan-2_1/controls/Mode/on1",
        "1");
    const auto unsupported = commands.ProcessOne();
    Require(unsupported.has_value(), "unsupported MQTT command was lost");
    Require(
        unsupported->status == mdv::MqttCommandStatus::InvalidPayload,
        "profile-disabled Mode command was not rejected");
    Require(
        transport.requests.size() == trafficBeforeUnsupported,
        "unsupported MQTT command generated Modbus traffic");

    const auto publicationsBeforeOffline = mqtt.publications.size();
    const auto offline = driver.ProcessNext();
    states.PublishAfter(driver, offline);
    Require(
        offline.operation == mdv::DriverOperation::PollRead &&
            offline.outcome == mdv::DriverOutcome::Timeout,
        "zero presence probe did not produce ordinary offline outcome");
    Require(
        mqtt.HasPublication(
            "/devices/Fan-2_1/controls/Alarm",
            "2",
            publicationsBeforeOffline),
        "Modbus offline state did not publish Alarm=2");
    Require(
        mqtt.HasPublication(
            "/devices/Fan-2_1/controls/Status",
            "7",
            publicationsBeforeOffline),
        "Modbus offline state did not publish Status=7");
}

} // namespace

int main()
{
    try {
        TestModbusStateAndPowerCommandUseExistingMqttBoundary();
        std::cout << "MDVWB Modbus MQTT integration tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus MQTT integration tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
