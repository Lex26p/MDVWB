#include "mdvwb_manager_mqtt.h"

#include "mdv_buses_config.h"
#include "mdv_mosquitto.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace mdvwb {
namespace {

constexpr std::size_t MaximumConfigPayloadBytes = 64U * 1024U;
constexpr std::string_view BusTopicPrefix = "/mdvwb/buses/";
std::atomic_bool StopRequested = false;

std::string JsonEscape(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 8U);
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20U) {
                result += '?';
            } else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return result;
}

std::size_t EnabledCount(const BusesConfig& config) {
    return static_cast<std::size_t>(std::count_if(
        config.buses.begin(), config.buses.end(),
        [](const BusConfig& bus) { return bus.enabled; }));
}

const BusConfig* FindBus(const BusesConfig& config, int busId) {
    const auto iterator = std::find_if(
        config.buses.begin(), config.buses.end(),
        [busId](const BusConfig& bus) { return bus.id == busId; });
    return iterator == config.buses.end() ? nullptr : &*iterator;
}

std::string JoinAddressesAsJson(const std::vector<int>& addresses) {
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << addresses[index];
    }
    output << ']';
    return output.str();
}

std::string BusStatusTopic(int busId) {
    return "/mdvwb/buses/" + std::to_string(busId) + "/status";
}

std::string BusResultTopic(int busId) {
    return "/mdvwb/buses/" + std::to_string(busId) + "/result";
}

std::string DiscoveryStatusTopic(int busId) {
    return "/mdvwb/buses/" + std::to_string(busId) + "/discovery/status";
}

std::string DiscoveryResultTopic(int busId) {
    return "/mdvwb/buses/" + std::to_string(busId) + "/discovery/result";
}

int ReadIntegerEnvironment(const char* name, int fallback) {
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
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("invalid integer in ") + name);
    }
}

std::string ReadStringEnvironment(const char* name, std::string fallback = {}) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    return value;
}

mdv::MqttConnectionOptions ManagerMqttOptionsFromEnvironment() {
    mdv::MqttConnectionOptions options;
    options.host = ReadStringEnvironment("MDVWB_MQTT_HOST", "127.0.0.1");
    options.port = ReadIntegerEnvironment("MDVWB_MQTT_PORT", 1883);
    options.keepAliveSeconds = ReadIntegerEnvironment("MDVWB_MQTT_KEEPALIVE", 60);
    options.clientId = "mdvwb-manager";
    options.username = ReadStringEnvironment("MDVWB_MQTT_USER");
    options.password = ReadStringEnvironment("MDVWB_MQTT_PASSWORD");
    options.reconnectDelaySeconds = static_cast<unsigned int>(
        ReadIntegerEnvironment("MDVWB_MQTT_RECONNECT", 1));
    options.reconnectDelayMaxSeconds = static_cast<unsigned int>(
        ReadIntegerEnvironment("MDVWB_MQTT_RECONNECT_MAX", 10));
    return options;
}

void HandleStopSignal(int) {
    StopRequested.store(true);
}

}  // namespace

ManagerMqttService::ManagerMqttService(
    mdv::IMqttClient& client,
    std::filesystem::path configPath,
    ServiceSyncPaths servicePaths,
    CommandRunner& commandRunner,
    DiscoveryRunner* discoveryRunner)
    : client_(client),
      configPath_(std::move(configPath)),
      servicePaths_(std::move(servicePaths)),
      commandRunner_(commandRunner),
      discoveryRunner_(discoveryRunner) {
    if (configPath_.empty()) {
        throw std::invalid_argument("manager MQTT configuration path cannot be empty");
    }
}

void ManagerMqttService::Start() {
    if (started_) {
        return;
    }
    client_.SetMessageHandler([this](mdv::MqttMessage message) {
        Enqueue(std::move(message));
    });
    client_.Subscribe(ConfigSetTopic);
    client_.Subscribe(BusStartFilter);
    client_.Subscribe(BusStopFilter);
    client_.Subscribe(BusRestartFilter);
    client_.Subscribe(BusStatusGetFilter);
    client_.Subscribe(BusDiscoveryStartFilter);
    started_ = true;

    try {
        PublishCurrentConfig();
    } catch (const std::exception& error) {
        PublishErrorStatus(error.what());
    }
}

std::optional<ManagerMqttService::IncomingCommand> ManagerMqttService::ParseIncoming(
    mdv::MqttMessage message) {
    if (message.topic == ConfigSetTopic) {
        return IncomingCommand{IncomingType::Configuration, std::nullopt, std::move(message)};
    }
    if (message.topic.rfind(BusTopicPrefix, 0) != 0U) {
        return std::nullopt;
    }

    const std::string_view remainder(message.topic.data() + BusTopicPrefix.size(),
                                     message.topic.size() - BusTopicPrefix.size());
    const std::size_t separator = remainder.find('/');
    if (separator == std::string_view::npos || separator == 0U) {
        return std::nullopt;
    }

    int busId = 0;
    const std::string_view idText = remainder.substr(0, separator);
    const auto parse = std::from_chars(idText.data(), idText.data() + idText.size(), busId);
    if (parse.ec != std::errc{} || parse.ptr != idText.data() + idText.size() ||
        busId < 1 || busId > 999) {
        return std::nullopt;
    }

    const std::string_view suffix = remainder.substr(separator);
    IncomingType type;
    if (suffix == "/start") {
        type = IncomingType::BusStart;
    } else if (suffix == "/stop") {
        type = IncomingType::BusStop;
    } else if (suffix == "/restart") {
        type = IncomingType::BusRestart;
    } else if (suffix == "/status/get") {
        type = IncomingType::BusStatus;
    } else if (suffix == "/discovery/start") {
        type = IncomingType::BusDiscovery;
    } else {
        return std::nullopt;
    }
    return IncomingCommand{type, busId, std::move(message)};
}

void ManagerMqttService::Enqueue(mdv::MqttMessage message) {
    std::optional<IncomingCommand> command = ParseIncoming(std::move(message));
    if (!command.has_value()) {
        return;
    }
    std::lock_guard lock(mutex_);
    inbox_.push_back(std::move(*command));
}

std::optional<ManagerMqttResult> ManagerMqttService::ProcessOne() {
    IncomingCommand command;
    {
        std::lock_guard lock(mutex_);
        if (inbox_.empty()) {
            return std::nullopt;
        }
        command = std::move(inbox_.front());
        inbox_.pop_front();
    }

    if (command.type == IncomingType::Configuration) {
        return ProcessConfiguration(command.message);
    }
    if (command.type == IncomingType::BusDiscovery) {
        return ProcessDiscovery(*command.busId, command.message);
    }
    return ProcessBusCommand(command.type, *command.busId, command.message);
}

ManagerMqttResult ManagerMqttService::ProcessConfiguration(
    const mdv::MqttMessage& message) {
    if (message.retained) {
        constexpr std::string_view error =
            "Retained configuration commands are ignored";
        PublishResult(false, false, error);
        return ManagerMqttResult{false, false, std::string(error), std::nullopt, {}};
    }
    if (message.payload.size() > MaximumConfigPayloadBytes) {
        constexpr std::string_view error =
            "Configuration payload exceeds 65536 bytes";
        PublishResult(false, false, error);
        return ManagerMqttResult{false, false, std::string(error), std::nullopt, {}};
    }

    try {
        std::optional<BusesConfig> previousConfig;
        try {
            previousConfig = LoadBusesConfig(configPath_);
        } catch (const std::exception&) {
            // A valid incoming configuration can recover a missing or damaged file.
        }

        const BusesConfig config = ParseBusesConfig(message.payload);
        const std::string canonical = SerializeBusesConfig(config);
        const std::size_t enabledCount = EnabledCount(config);

        WriteTextFileAtomically(configPath_, canonical);
        client_.Publish(ConfigTopic, canonical, true);
        if (previousConfig.has_value()) {
            RemoveObsoleteDeviceTopics(*previousConfig, config);
        }

        try {
            const ServiceSyncPlan plan = BuildServiceSyncPlan(config, servicePaths_);
            ApplyServiceSyncPlan(plan, servicePaths_, commandRunner_);
            PublishReadyStatus(config.buses.size(), enabledCount);
            PublishAllBusStatuses(config);
            PublishDiscoveryIdleStatuses(config);
            PublishResult(
                true,
                true,
                "Configuration saved and applied",
                config.buses.size(),
                enabledCount,
                plan.actions.size());
            return ManagerMqttResult{
                true, true, "Configuration saved and applied", std::nullopt, {}};
        } catch (const std::exception& error) {
            const std::string detail =
                std::string("Configuration saved, service synchronization failed: ") +
                error.what();
            PublishErrorStatus(detail);
            PublishResult(
                false,
                true,
                detail,
                config.buses.size(),
                enabledCount);
            return ManagerMqttResult{false, true, detail, std::nullopt, {}};
        }
    } catch (const BusesConfigError& error) {
        const std::string detail = std::string("Invalid configuration: ") + error.what();
        PublishResult(false, false, detail);
        return ManagerMqttResult{false, false, detail, std::nullopt, {}};
    } catch (const std::exception& error) {
        const std::string detail = std::string("Cannot save configuration: ") + error.what();
        PublishErrorStatus(detail);
        PublishResult(false, false, detail);
        return ManagerMqttResult{false, false, detail, std::nullopt, {}};
    }
}

ManagerMqttResult ManagerMqttService::ProcessBusCommand(
    IncomingType type,
    int busId,
    const mdv::MqttMessage& message) {
    std::string command;
    BusServiceCommand serviceCommand = BusServiceCommand::Start;
    switch (type) {
        case IncomingType::BusStart:
            command = "start";
            serviceCommand = BusServiceCommand::Start;
            break;
        case IncomingType::BusStop:
            command = "stop";
            serviceCommand = BusServiceCommand::Stop;
            break;
        case IncomingType::BusRestart:
            command = "restart";
            serviceCommand = BusServiceCommand::Restart;
            break;
        case IncomingType::BusStatus:
            command = "status";
            break;
        case IncomingType::BusDiscovery:
            throw std::logic_error("discovery command routed as service command");
        case IncomingType::Configuration:
            throw std::logic_error("configuration command routed as bus command");
    }

    if (message.retained) {
        const std::string detail = "Retained bus commands are ignored";
        PublishBusResult(busId, command, false, detail);
        return ManagerMqttResult{false, false, detail, busId, command};
    }

    try {
        const BusesConfig config = LoadBusesConfig(configPath_);
        const BusConfig* bus = FindBus(config, busId);
        if (bus == nullptr) {
            const std::string detail = "Bus is not configured";
            PublishBusResult(busId, command, false, detail);
            return ManagerMqttResult{false, false, detail, busId, command};
        }

        if ((type == IncomingType::BusStart || type == IncomingType::BusRestart) &&
            !bus->enabled) {
            const std::string detail = "Bus is disabled in configuration";
            PublishBusStatus(*bus);
            PublishBusResult(busId, command, false, detail);
            return ManagerMqttResult{false, false, detail, busId, command};
        }

        if (type != IncomingType::BusStatus) {
            ExecuteBusServiceCommand(busId, serviceCommand, servicePaths_, commandRunner_);
        }
        PublishBusStatus(*bus);

        std::string detail;
        if (type == IncomingType::BusStart) {
            detail = "Bus service started";
        } else if (type == IncomingType::BusStop) {
            detail = "Bus service stopped";
        } else if (type == IncomingType::BusRestart) {
            detail = "Bus service restarted";
        } else {
            detail = "Bus status published";
        }
        PublishBusResult(busId, command, true, detail);
        return ManagerMqttResult{true, false, detail, busId, command};
    } catch (const std::exception& error) {
        const std::string detail =
            std::string("Bus command failed: ") + error.what();
        PublishBusResult(busId, command, false, detail);
        return ManagerMqttResult{false, false, detail, busId, command};
    }
}


ManagerMqttResult ManagerMqttService::ProcessDiscovery(
    int busId,
    const mdv::MqttMessage& message) {
    constexpr std::string_view command = "discovery";
    if (message.retained) {
        const std::string detail = "Retained discovery commands are ignored";
        PublishBusResult(busId, command, false, detail);
        PublishDiscoveryStatus(busId, "error", {}, detail);
        return ManagerMqttResult{false, false, detail, busId, std::string(command)};
    }
    if (discoveryRunner_ == nullptr) {
        const std::string detail = "Discovery runner is not configured";
        PublishBusResult(busId, command, false, detail);
        PublishDiscoveryStatus(busId, "error", {}, detail);
        return ManagerMqttResult{false, false, detail, busId, std::string(command)};
    }

    try {
        const BusesConfig config = LoadBusesConfig(configPath_);
        const BusConfig* bus = FindBus(config, busId);
        if (bus == nullptr) {
            const std::string detail = "Bus is not configured";
            PublishBusResult(busId, command, false, detail);
            PublishDiscoveryStatus(busId, "error", {}, detail);
            return ManagerMqttResult{false, false, detail, busId, std::string(command)};
        }

        PublishDiscoveryStatus(busId, "running", bus->port, "Discovery is running");
        const BusServiceStatus serviceStatus =
            QueryBusServiceStatus(busId, servicePaths_, commandRunner_);
        if (serviceStatus.active) {
            ExecuteBusServiceCommand(
                busId, BusServiceCommand::Stop, servicePaths_, commandRunner_);
        }
        PublishBusStatus(*bus);

        constexpr int MasterId = 0;
        constexpr int PeriodMilliseconds = 150;
        constexpr int ResponseTimeoutMilliseconds = 130;
        const DiscoveryExecutionResult discovery = discoveryRunner_->Run(
            bus->port,
            MasterId,
            PeriodMilliseconds,
            ResponseTimeoutMilliseconds);

        if (!discovery.success) {
            const std::string detail = discovery.message.empty()
                ? "Discovery failed"
                : discovery.message;
            PublishDiscoveryResult(busId, false, {}, detail);
            PublishDiscoveryStatus(busId, "error", bus->port, detail);
            PublishBusResult(busId, command, false, detail);
            return ManagerMqttResult{false, false, detail, busId, std::string(command)};
        }

        const std::string detail = discovery.addresses.empty()
            ? "Discovery completed; no devices found"
            : "Discovery completed";
        PublishDiscoveryResult(busId, true, discovery.addresses, detail);
        PublishDiscoveryStatus(
            busId, "completed", bus->port, detail, discovery.addresses.size());
        PublishBusResult(busId, command, true, detail);
        return ManagerMqttResult{true, false, detail, busId, std::string(command)};
    } catch (const std::exception& error) {
        const std::string detail = std::string("Discovery failed: ") + error.what();
        PublishDiscoveryResult(busId, false, {}, detail);
        PublishDiscoveryStatus(busId, "error", {}, detail);
        PublishBusResult(busId, command, false, detail);
        return ManagerMqttResult{false, false, detail, busId, std::string(command)};
    }
}

std::size_t ManagerMqttService::PendingCount() const {
    std::lock_guard lock(mutex_);
    return inbox_.size();
}

void ManagerMqttService::PublishCurrentConfig() {
    const BusesConfig config = LoadBusesConfig(configPath_);
    client_.Publish(ConfigTopic, SerializeBusesConfig(config), true);
    PublishReadyStatus(config.buses.size(), EnabledCount(config));
    PublishAllBusStatuses(config);
    PublishDiscoveryIdleStatuses(config);
}

void ManagerMqttService::PublishReadyStatus(
    std::size_t busCount,
    std::size_t enabledCount) {
    const std::string payload =
        "{\"state\":\"ready\",\"buses\":" + std::to_string(busCount) +
        ",\"enabled\":" + std::to_string(enabledCount) + "}";
    client_.Publish(StatusTopic, payload, true);
}

void ManagerMqttService::PublishErrorStatus(std::string_view message) {
    const std::string payload =
        "{\"state\":\"error\",\"message\":\"" + JsonEscape(message) + "\"}";
    client_.Publish(StatusTopic, payload, true);
}

void ManagerMqttService::PublishResult(
    bool success,
    bool saved,
    std::string_view message,
    std::size_t busCount,
    std::size_t enabledCount,
    std::size_t actionCount) {
    const std::string payload =
        std::string("{\"success\":") + (success ? "true" : "false") +
        ",\"saved\":" + (saved ? "true" : "false") +
        ",\"message\":\"" + JsonEscape(message) + "\"" +
        ",\"buses\":" + std::to_string(busCount) +
        ",\"enabled\":" + std::to_string(enabledCount) +
        ",\"actions\":" + std::to_string(actionCount) + "}";
    client_.Publish(ConfigResultTopic, payload, false);
}

void ManagerMqttService::PublishAllBusStatuses(const BusesConfig& config) {
    for (const BusConfig& bus : config.buses) {
        PublishBusStatus(bus);
    }
}

void ManagerMqttService::PublishBusStatus(const BusConfig& bus) {
    const BusServiceStatus status =
        QueryBusServiceStatus(bus.id, servicePaths_, commandRunner_);
    const std::string payload =
        "{\"id\":" + std::to_string(bus.id) +
        ",\"configured\":true" +
        ",\"enabled\":" + (bus.enabled ? std::string("true") : std::string("false")) +
        ",\"service\":\"" + (status.active ? std::string("active") : std::string("inactive")) + "\"" +
        ",\"autostart\":" + (status.enabled ? std::string("true") : std::string("false")) +
        ",\"port\":\"" + JsonEscape(bus.port) + "\"" +
        ",\"addresses\":" + JoinAddressesAsJson(bus.addresses) + "}";
    client_.Publish(BusStatusTopic(bus.id), payload, true);
}

void ManagerMqttService::PublishBusResult(
    int busId,
    std::string_view command,
    bool success,
    std::string_view message) {
    const std::string payload =
        std::string("{\"success\":") + (success ? "true" : "false") +
        ",\"bus\":" + std::to_string(busId) +
        ",\"command\":\"" + JsonEscape(command) + "\"" +
        ",\"message\":\"" + JsonEscape(message) + "\"}";
    client_.Publish(BusResultTopic(busId), payload, false);
}


void ManagerMqttService::PublishDiscoveryStatus(
    int busId,
    std::string_view state,
    std::string_view port,
    std::string_view message,
    std::size_t foundCount) {
    std::string payload =
        "{\"bus\":" + std::to_string(busId) +
        ",\"state\":\"" + JsonEscape(state) + "\"";
    if (!port.empty()) {
        payload += ",\"port\":\"" + JsonEscape(port) + "\"";
    }
    if (!message.empty()) {
        payload += ",\"message\":\"" + JsonEscape(message) + "\"";
    }
    if (state == "completed") {
        payload += ",\"found\":" + std::to_string(foundCount);
    }
    payload += '}';
    client_.Publish(DiscoveryStatusTopic(busId), payload, true);
}

void ManagerMqttService::PublishDiscoveryResult(
    int busId,
    bool success,
    const std::vector<int>& addresses,
    std::string_view message) {
    const std::string payload =
        std::string("{\"success\":") + (success ? "true" : "false") +
        ",\"bus\":" + std::to_string(busId) +
        ",\"addresses\":" + JoinAddressesAsJson(addresses) +
        ",\"message\":\"" + JsonEscape(message) + "\"}";
    client_.Publish(DiscoveryResultTopic(busId), payload, true);
}

void ManagerMqttService::PublishDiscoveryIdleStatuses(const BusesConfig& config) {
    for (const BusConfig& bus : config.buses) {
        PublishDiscoveryStatus(bus.id, "idle", bus.port);
    }
}


void ManagerMqttService::RemoveObsoleteDeviceTopics(
    const BusesConfig& previous,
    const BusesConfig& current) {
    for (const BusConfig& oldBus : previous.buses) {
        const BusConfig* newBus = FindBus(current, oldBus.id);
        for (const int oldAddress : oldBus.addresses) {
            const bool remains = newBus != nullptr &&
                std::find(newBus->addresses.begin(), newBus->addresses.end(), oldAddress) !=
                    newBus->addresses.end();
            if (!remains) {
                ClearFanDeviceTopics(oldBus.id, oldAddress);
            }
        }
        if (newBus == nullptr) {
            ClearSystemDeviceTopics(oldBus.id);
            client_.Publish(BusStatusTopic(oldBus.id), "", true);
            client_.Publish(DiscoveryStatusTopic(oldBus.id), "", true);
            client_.Publish(DiscoveryResultTopic(oldBus.id), "", true);
        }
    }
}

void ManagerMqttService::ClearFanDeviceTopics(int busId, int address) {
    static constexpr std::string_view Controls[] = {
        "Alarm", "AlarmCode", "Blinds", "Blok", "Mode",
        "Power", "SetTemp", "Speed", "Status", "Temp"};
    static constexpr std::string_view Metadata[] = {
        "title", "type", "order", "readonly", "max", "error"};

    const std::string prefix = "/devices/Fan-" + std::to_string(busId) + "_" +
        std::to_string(address);
    ClearRetained(prefix + "/meta/name");
    ClearRetained(prefix + "/meta/driver");
    ClearRetained(prefix + "/meta/error");
    for (const std::string_view control : Controls) {
        const std::string controlPrefix = prefix + "/controls/" + std::string(control);
        ClearRetained(controlPrefix);
        ClearRetained(controlPrefix + "/on1");
        for (const std::string_view metadata : Metadata) {
            ClearRetained(controlPrefix + "/meta/" + std::string(metadata));
        }
    }
}

void ManagerMqttService::ClearSystemDeviceTopics(int busId) {
    static constexpr std::string_view Controls[] = {"Serial", "Error", "GanGetID"};
    static constexpr std::string_view Metadata[] = {
        "title", "type", "order", "readonly", "max", "error"};

    const std::string prefix = "/devices/sist-" + std::to_string(busId);
    ClearRetained(prefix + "/meta/name");
    ClearRetained(prefix + "/meta/driver");
    ClearRetained(prefix + "/meta/error");
    for (const std::string_view control : Controls) {
        const std::string controlPrefix = prefix + "/controls/" + std::string(control);
        ClearRetained(controlPrefix);
        for (const std::string_view metadata : Metadata) {
            ClearRetained(controlPrefix + "/meta/" + std::string(metadata));
        }
    }
}

void ManagerMqttService::ClearRetained(std::string topic) {
    client_.Publish(topic, "", true);
}

int RunManagerMqttDaemon(
    const std::filesystem::path& configPath,
    std::ostream& output,
    std::ostream& errors) {
    if (!mdv::MosquittoMqttClient::IsSupported()) {
        errors << "MANAGER_ERROR: libmosquitto support is not available in this build\n";
        return 1;
    }

    try {
        StopRequested.store(false);
        std::signal(SIGINT, HandleStopSignal);
        std::signal(SIGTERM, HandleStopSignal);

        mdv::MosquittoMqttClient client(ManagerMqttOptionsFromEnvironment());
        NativeCommandRunner commandRunner;
        const ServiceSyncPaths servicePaths = ServiceSyncPathsFromEnvironment();
        const BusesConfig startupConfig = LoadBusesConfig(configPath);
        const ServiceSyncPlan startupPlan =
            BuildServiceSyncPlan(startupConfig, servicePaths);
        ApplyServiceSyncPlan(startupPlan, servicePaths, commandRunner);
        output << "MQTT_MANAGER_SYNC actions=" << startupPlan.actions.size() << '\n';

        NativeDiscoveryRunner discoveryRunner(
            ReadStringEnvironment("MDVWB_BINARY", "/usr/local/bin/MDVWB"));
        ManagerMqttService service(
            client,
            configPath,
            servicePaths,
            commandRunner,
            &discoveryRunner);

        service.Start();
        client.Start();
        output << "MQTT_MANAGER_STARTED config=" << configPath.string() << '\n';

        while (!StopRequested.load()) {
            while (service.ProcessOne().has_value()) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        client.Stop();
        output << "MQTT_MANAGER_STOPPED\n";
        return 0;
    } catch (const std::exception& error) {
        errors << "MANAGER_ERROR: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace mdvwb
