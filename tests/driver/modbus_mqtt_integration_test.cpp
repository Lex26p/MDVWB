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

[[nodiscard]] std::uint16_t RequestQuantity(
    const mdv::modbus::RtuAdu& request)
{
    Require(request.size() >= 6U, "Modbus request has no quantity");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(request[4]) << 8U) |
        static_cast<std::uint16_t>(request[5]));
}

[[nodiscard]] std::uint16_t RequestWriteValue(
    const mdv::modbus::RtuAdu& request)
{
    Require(request.size() == 11U, "one-register FC10 request size mismatch");
    return static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(request[7]) << 8U) |
        static_cast<std::uint16_t>(request[8]));
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

[[nodiscard]] mdv::modbus::TransactionResult ReadSuccess(
    const mdv::modbus::RtuAdu& request,
    std::vector<std::uint16_t> values)
{
    mdv::modbus::ParsedResponse response;
    response.status = mdv::modbus::ResponseStatus::Success;
    response.slaveId = request[0];
    response.function = mdv::modbus::Function::ReadHoldingRegisters;
    response.registers = std::move(values);

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
    response.quantity = static_cast<std::uint16_t>(1U);

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
            Require(RequestQuantity(request) == 1U, "FC10 quantity mismatch");
            const auto value = RequestWriteValue(request);
            switch (address) {
            case 40078U: powerRaw = value; break;
            case 40079U: modeRaw = value; break;
            case 40080U: fanSpeedRaw = value; break;
            case 40081U: setTemperatureRaw = value; break;
            default:
                throw std::runtime_error(
                    "unexpected Modbus write register " +
                    std::to_string(address));
            }
            ++writeCount;
            return WriteSuccess(request);
        }

        Require(
            function == static_cast<std::uint8_t>(
                mdv::modbus::Function::ReadHoldingRegisters),
            "unexpected Modbus function in MQTT integration test");

        if (address == 40039U) {
            ++probeCount;
            // Initial snapshot sees the device. Later ordinary polls report
            // absence so MQTT can verify the three-failure offline threshold.
            return ReadSuccess(request, probeCount == 1U ? 24U : 0U);
        }
        if (address == 40028U) {
            ++powerReadCount;
            if (RequestQuantity(request) == 4U) {
                return ReadSuccess(
                    request,
                    std::vector<std::uint16_t>{
                        powerRaw,
                        modeRaw,
                        fanSpeedRaw,
                        setTemperatureRaw});
            }
            return ReadSuccess(request, powerRaw);
        }
        if (address == 40029U) return ReadSuccess(request, modeRaw);
        if (address == 40030U) return ReadSuccess(request, fanSpeedRaw);
        if (address == 40031U) return ReadSuccess(request, setTemperatureRaw);
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
    std::uint16_t powerRaw = 0U;
    std::uint16_t modeRaw = 2U;
    std::uint16_t fanSpeedRaw = 1U;
    std::uint16_t setTemperatureRaw = 24U;
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

void TestModbusStateAndCommandsUseExistingMqttBoundary()
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
    Require(
        mqtt.HasPublication(
            "/devices/Fan-2_1/controls/Mode", "0") &&
            mqtt.HasPublication(
                "/devices/Fan-2_1/controls/Speed", "4") &&
            mqtt.HasPublication(
                "/devices/Fan-2_1/controls/SetTemp", "24") &&
            mqtt.HasPublication(
                "/devices/Fan-2_1/controls/Status", "0"),
        "initial Modbus HVAC state was not published factually");

    const auto beforeForcedSnapshot = mqtt.publications.size();
    states.PublishDevice(driver.DeviceStateByAddress(1U), true);
    for (const std::string_view control : {"Blinds", "Blok"}) {
        Require(
            mqtt.HasPublication(
                "/devices/Fan-2_1/controls/" + std::string(control),
                "",
                beforeForcedSnapshot),
            "forced Modbus snapshot did not clear an unsupported retained control");
    }

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
    Require(
        mqtt.HasPublication(
            "/devices/Fan-2_1/controls/Status",
            "1",
            publicationsBeforeWrite),
        "confirmed Power did not publish cooling Status=1");

    const auto trafficBeforeMode = transport.requests.size();
    mqtt.Emit(
        "/devices/Fan-2_1/controls/Mode/on1",
        "1");
    const auto modeCommand = commands.ProcessOne();
    Require(modeCommand.has_value(), "Mode MQTT command was lost");
    Require(
        modeCommand->status == mdv::MqttCommandStatus::Applied,
        "Mode MQTT command was not accepted");
    Require(
        transport.requests.size() == trafficBeforeMode,
        "Mode routing generated Modbus traffic outside driver thread");

    const auto modeWrite = driver.ProcessNext();
    states.PublishAfter(driver, modeWrite);
    Require(
        modeWrite.operation == mdv::DriverOperation::SetState &&
            modeWrite.outcome == mdv::DriverOutcome::Success &&
            RequestAddress(transport.requests.back()) == 40079U &&
            RequestWriteValue(transport.requests.back()) == 16U,
        "Mode Heat was not written as the profile raw value");

    const auto modeConfirmation = driver.ProcessNext();
    const auto publicationsBeforeModeConfirmation = mqtt.publications.size();
    states.PublishAfter(driver, modeConfirmation);
    Require(
        modeConfirmation.operation == mdv::DriverOperation::ConfirmRead &&
            modeConfirmation.outcome == mdv::DriverOutcome::Success,
        "Mode confirmation read failed");
    Require(
        mqtt.HasPublication(
            "/devices/Fan-2_1/controls/Mode",
            "1",
            publicationsBeforeModeConfirmation) &&
            mqtt.HasPublication(
                "/devices/Fan-2_1/controls/Status",
                "2",
                publicationsBeforeModeConfirmation),
        "confirmed Heat mode did not publish Mode=1 and Status=2");

    const auto publicationsBeforeOffline = mqtt.publications.size();
    for (std::uint32_t failure = 1;
         failure < mdv::modbus::kModbusPollFailuresBeforeOffline;
         ++failure) {
        const auto transientFailure = driver.ProcessNext();
        states.PublishAfter(driver, transientFailure);
        Require(
            transientFailure.operation == mdv::DriverOperation::PollRead &&
                transientFailure.outcome == mdv::DriverOutcome::Timeout,
            "zero presence probe did not produce ordinary offline outcome");
        Require(
            driver.DeviceStateByAddress(1U).online,
            "transient Modbus failure marked the device offline");
        Require(
            mqtt.publications.size() == publicationsBeforeOffline,
            "transient Modbus failure published a false offline state");
    }

    const auto offline = driver.ProcessNext();
    states.PublishAfter(driver, offline);
    Require(
        offline.operation == mdv::DriverOperation::PollRead &&
            offline.outcome == mdv::DriverOutcome::Timeout &&
            !driver.DeviceStateByAddress(1U).online,
        "third zero presence probe did not mark the device offline");
    Require(
        offline.error.find("stage=probe") != std::string::npos &&
            offline.error.find("register=40039") != std::string::npos &&
            offline.error.find("consecutive-poll-failures=3/3") !=
                std::string::npos,
        "offline diagnostic lacks stage, register or failure counter");
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
        TestModbusStateAndCommandsUseExistingMqttBoundary();
        std::cout << "MDVWB Modbus MQTT integration tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus MQTT integration tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
