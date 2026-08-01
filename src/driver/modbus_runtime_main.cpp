#include "modbus_runtime_config.h"

#include "mdv_metadata.h"
#include "mdv_mosquitto.h"
#include "mdv_mqtt.h"
#include "modbus_driver.h"
#include "modbus_rtu_serial.h"
#include "modbus_runtime_cadence.h"

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#ifndef MDVWB_VERSION
#define MDVWB_VERSION "development"
#endif

namespace {

volatile std::sig_atomic_t gStopRequested = 0;

void RequestStop(int) noexcept
{
    gStopRequested = 1;
}

[[nodiscard]] std::optional<std::string> ProcessEnvironment(
    std::string_view name)
{
    const std::string key(name);
    const char* value = std::getenv(key.c_str());
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

[[nodiscard]] mdv::MqttConnectionOptions MqttOptions(
    const mdv::modbus::RuntimeMqttSettings& settings)
{
    mdv::MqttConnectionOptions result;
    result.host = settings.host;
    result.port = settings.port;
    result.keepAliveSeconds = settings.keepAliveSeconds;
    result.clientId = settings.clientId;
    result.username = settings.username;
    result.password = settings.password;
    result.reconnectDelaySeconds = settings.reconnectDelaySeconds;
    result.reconnectDelayMaxSeconds = settings.reconnectDelayMaxSeconds;
    return result;
}

[[nodiscard]] std::chrono::milliseconds InitialSnapshotDelay(
    const mdv::modbus::ModbusRuntimeConfig& config,
    const mdv::modbus::ModbusPollPlanMetrics& metrics)
{
    const auto transactionBudget = config.responseTimeout *
        static_cast<std::int64_t>(
            std::max<std::size_t>(
                1U,
                metrics.optimizedTotalTransactionsPerCycle));
    const auto schedulingBudget = config.cadence.pollPeriod *
        static_cast<std::int64_t>(config.addresses.size());
    return std::max(
        std::chrono::milliseconds{3000},
        transactionBudget + schedulingBudget +
            std::chrono::milliseconds{1000});
}

int RunModbusRuntime()
{
    if (!mdv::MosquittoMqttClient::IsSupported()) {
        throw std::runtime_error(
            "this build has no libmosquitto support; install the development library and rebuild");
    }

    const auto config = mdv::modbus::ParseModbusRuntimeConfig(
        ProcessEnvironment);
    const auto profile = mdv::modbus::LoadModbusRuntimeProfile(config);

    mdv::modbus::RtuSerialTransport transport(
        config.serial,
        mdv::modbus::RtuTimingSettings{
            .responseTimeout = config.responseTimeout,
        });
    transport.Open(config.serialPort);

    mdv::modbus::ModbusDriver driver(
        config.addresses,
        profile,
        transport,
        config.driverPolicy);
    mdv::MosquittoMqttClient mqtt(MqttOptions(config.mqtt));
    mdv::MqttCommandRouter router(config.busNumber, driver);
    mdv::MqttCommandService commandService(mqtt, router);
    mdv::MqttStatePublisher statePublisher(config.busNumber, mqtt);
    mdv::MqttMetadataPublisher metadataPublisher(config.busNumber, mqtt);
    mdv::MqttSystemPublisher systemPublisher(
        config.busNumber,
        mqtt,
        config.publishPollAddress);

    commandService.Start();
    mqtt.Start();
    systemPublisher.PublishSerial("Modbus RTU порт открыт", true);
    systemPublisher.PublishError("", true);

    std::signal(SIGINT, RequestStop);
    std::signal(SIGTERM, RequestStop);

    const auto& metrics = driver.PollPlanMetrics();
    std::cout
        << "MDVWB Modbus runtime started: port=" << config.serialPort
        << ", bus=" << config.busNumber
        << ", profile=" << profile.id
        << ", addresses=";
    for (std::size_t index = 0; index < config.addresses.size(); ++index) {
        if (index != 0U) {
            std::cout << ',';
        }
        std::cout << static_cast<int>(config.addresses[index]);
    }
    std::cout
        << ", serial=" << config.serial.baudRate << '/'
        << static_cast<int>(config.serial.dataBits) << '/'
        << static_cast<int>(config.serial.stopBits)
        << ", response-timeout=" << config.responseTimeout.count() << " ms"
        << ", cadence=" << config.cadence.pollPeriod.count() << '/'
        << config.cadence.commandPeriod.count() << '/'
        << config.cadence.retryPeriod.count() << " ms"
        << ", retries=" << config.driverPolicy.maxWriteAttempts << '/'
        << config.driverPolicy.maxConfirmationAttempts
        << ", priority-burst="
        << config.driverPolicy.maxPriorityOperationsBeforePoll
        << ", poll-transactions=" << metrics.totalTransactionsPerCycle
        << "->" << metrics.optimizedTotalTransactionsPerCycle
        << ", MQTT=" << config.mqtt.host << ':' << config.mqtt.port << '\n';

    bool mqttWasConnected = false;
    std::optional<std::chrono::steady_clock::time_point> initialSnapshotAt;
    const auto initialSnapshotDelay = InitialSnapshotDelay(config, metrics);

    while (gStopRequested == 0) {
        const bool mqttConnected = mqtt.IsConnected();
        if (mqttConnected && !mqttWasConnected) {
            metadataPublisher.Publish(config.addresses);
            initialSnapshotAt =
                std::chrono::steady_clock::now() + initialSnapshotDelay;
        }
        mqttWasConnected = mqttConnected;

        // MQTT callbacks only enqueue messages. Keep command application and
        // every physical Modbus transaction on this one runtime thread.
        for (int count = 0; count < 64; ++count) {
            const auto command = commandService.ProcessOne();
            if (!command.has_value()) {
                break;
            }
            if (command->status != mdv::MqttCommandStatus::Applied &&
                command->status != mdv::MqttCommandStatus::Ignored) {
                std::cerr
                    << "Modbus MQTT command rejected: "
                    << command->error << '\n';
            }
        }

        const auto operationStarted = std::chrono::steady_clock::now();
        const auto result = driver.ProcessNext();
        statePublisher.PublishAfter(driver, result);
        systemPublisher.PublishAfter(result);

        if (mqtt.IsConnected() && initialSnapshotAt.has_value() &&
            std::chrono::steady_clock::now() >= *initialSnapshotAt) {
            for (const auto address : config.addresses) {
                statePublisher.PublishDevice(
                    driver.DeviceStateByAddress(address),
                    true);
            }
            systemPublisher.PublishSerial("Modbus RTU порт открыт", true);
            systemPublisher.PublishError("", true);
            initialSnapshotAt.reset();
            std::cout << "Modbus MQTT initial state snapshot published.\n";
        }

        const auto nextStart = operationStarted +
            mdv::modbus::ModbusOperationPeriod(result, config.cadence);
        const auto now = std::chrono::steady_clock::now();
        if (nextStart > now) {
            std::this_thread::sleep_until(nextStart);
        }
    }

    systemPublisher.PublishSerial("Порт закрыт");
    mqtt.Stop();
    transport.Close();
    std::cout << "MDVWB Modbus runtime stopped.\n";
    return 0;
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--version") {
            std::cout << "MDVWB Modbus runtime " << MDVWB_VERSION << '\n';
            return 0;
        }
        if (argc != 1) {
            std::cerr << "Usage: mdvwb-modbus [--version]\n";
            return 2;
        }
        return RunModbusRuntime();
    }
    catch (const std::invalid_argument& error) {
        std::cerr << "Modbus runtime configuration error: "
                  << error.what() << '\n';
        return 2;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus startup/runtime error: "
                  << error.what() << '\n';
        return 3;
    }
}
