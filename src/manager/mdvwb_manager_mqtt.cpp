#include "mdvwb_manager_mqtt.h"

#include "mdv_buses_config.h"
#include "mdv_mosquitto.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cctype>
#include <fstream>
#include <csignal>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace mdvwb {
namespace {

constexpr std::size_t MaximumConfigPayloadBytes = 64U * 1024U;
constexpr std::size_t MaximumDashboardPayloadBytes = 1024U * 1024U;
constexpr std::size_t MaximumSchedulesPayloadBytes = 1024U * 1024U;
constexpr auto DiscoveryInlineCompletionWait =
    std::chrono::milliseconds(100);
constexpr std::string_view BusTopicPrefix = "/mdvwb/buses/";
constexpr std::string_view ScheduleTopicPrefix = "/mdvwb/schedules/";
constexpr std::string_view BackgroundChunkPrefix =
    "/mdvwb/dashboard/background/upload/chunk/";
constexpr std::string_view BackgroundFinishPrefix =
    "/mdvwb/dashboard/background/upload/finish/";
constexpr std::string_view BackgroundCancelPrefix =
    "/mdvwb/dashboard/background/upload/cancel/";
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

std::size_t EnabledScheduleCount(const SchedulesConfig& config) {
    return static_cast<std::size_t>(std::count_if(
        config.schedules.begin(), config.schedules.end(),
        [](const ScheduleEntry& schedule) { return schedule.enabled; }));
}

const BusConfig* FindBus(const BusesConfig& config, int busId) {
    const auto iterator = std::find_if(
        config.buses.begin(), config.buses.end(),
        [busId](const BusConfig& bus) { return bus.id == busId; });
    return iterator == config.buses.end() ? nullptr : &*iterator;
}

std::string DashboardIssueKey(const DashboardReferenceIssue& issue) {
    return std::to_string(static_cast<int>(issue.kind)) + "|" +
        issue.panelId + "|" + issue.placementId + "|" +
        std::to_string(issue.bus) + "|" + std::to_string(issue.address);
}

std::string ScheduleIssueKey(const ScheduleReferenceIssue& issue) {
    return std::to_string(static_cast<int>(issue.kind)) + "|" +
        issue.scheduleId + "|" + issue.panelId + "|" +
        std::to_string(issue.bus) + "|" + std::to_string(issue.address);
}

template <typename Issue, typename KeyFunction>
std::size_t CountNewReferenceIssues(
    const std::vector<Issue>& current,
    const std::vector<Issue>& submitted,
    KeyFunction keyFunction) {
    std::set<std::string> existing;
    for (const Issue& issue : current) {
        existing.insert(keyFunction(issue));
    }

    return static_cast<std::size_t>(std::count_if(
        submitted.begin(),
        submitted.end(),
        [&](const Issue& issue) {
            return existing.find(keyFunction(issue)) == existing.end();
        }));
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

std::string ScheduleExecuteTopic(std::string_view scheduleId) {
    return "/mdvwb/schedules/" + std::string(scheduleId) + "/execute";
}

std::string ScheduleResultTopic(std::string_view scheduleId) {
    return "/mdvwb/schedules/" + std::string(scheduleId) + "/result";
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


std::string ComputeFileSha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read existing dashboard asset");
    }
    const std::string bytes{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return ComputeSha256Hex(bytes);
}
void HandleStopSignal(int) {
    StopRequested.store(true);
}

std::filesystem::path ResolveDashboardPath(
    const std::filesystem::path& configPath,
    std::filesystem::path dashboardPath) {
    return dashboardPath.empty()
        ? configPath.parent_path() / "dashboard.json"
        : std::move(dashboardPath);
}

std::filesystem::path ResolveDashboardAssetDirectory(
    const std::filesystem::path& dashboardPath,
    std::filesystem::path assetDirectory) {
    return assetDirectory.empty()
        ? dashboardPath.parent_path() / "assets"
        : std::move(assetDirectory);
}

std::filesystem::path ResolveSchedulesPath(
    const std::filesystem::path& configPath,
    std::filesystem::path schedulesPath) {
    return schedulesPath.empty()
        ? configPath.parent_path() / "schedules.json"
        : std::move(schedulesPath);
}

bool IsSafeUploadTopicId(std::string_view value) {
    return !value.empty() && value.size() <= 64U &&
        std::all_of(value.begin(), value.end(), [](char character) {
            const unsigned char byte = static_cast<unsigned char>(character);
            return std::isalnum(byte) != 0 || character == '-' || character == '_';
        });
}

bool IsManagedBackgroundFile(std::string_view fileName) {
    if (fileName.rfind("background-", 0U) != 0U ||
        fileName.find('/') != std::string_view::npos ||
        fileName.find('\\') != std::string_view::npos) {
        return false;
    }
    return std::all_of(fileName.begin(), fileName.end(), [](char character) {
        const unsigned char byte = static_cast<unsigned char>(character);
        return std::isalnum(byte) != 0 || character == '-' ||
            character == '_' || character == '.';
    });
}

}  // namespace

ManagerMqttService::ManagerMqttService(
    mdv::IMqttClient& client,
    std::filesystem::path configPath,
    ServiceSyncPaths servicePaths,
    CommandRunner& commandRunner,
    DiscoveryRunner* discoveryRunner,
    std::filesystem::path dashboardPath,
    std::filesystem::path dashboardAssetDirectory,
    std::filesystem::path schedulesPath)
    : client_(client),
      configPath_(std::move(configPath)),
      dashboardPath_(ResolveDashboardPath(configPath_, std::move(dashboardPath))),
      schedulesPath_(ResolveSchedulesPath(configPath_, std::move(schedulesPath))),
      backgroundUpload_(ResolveDashboardAssetDirectory(
          dashboardPath_, std::move(dashboardAssetDirectory))),
      servicePaths_(std::move(servicePaths)),
      commandRunner_(commandRunner),
      discoveryRunner_(discoveryRunner) {
    if (configPath_.empty()) {
        throw std::invalid_argument("manager MQTT configuration path cannot be empty");
    }
}

ManagerMqttService::~ManagerMqttService() {
    JoinDiscoveryWorker();
}

void ManagerMqttService::Start() {
    if (started_) {
        return;
    }
    client_.SetMessageHandler([this](mdv::MqttMessage message) {
        Enqueue(std::move(message));
    });
    client_.Subscribe(ConfigSetTopic);
    client_.Subscribe(DashboardConfigSetTopic);
    client_.Subscribe(SchedulesConfigSetTopic);
    client_.Subscribe(ScheduleRunFilter);
    client_.Subscribe(BackgroundUploadStartTopic);
    client_.Subscribe(BackgroundUploadChunkFilter);
    client_.Subscribe(BackgroundUploadFinishFilter);
    client_.Subscribe(BackgroundUploadCancelFilter);
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
    try {
        PublishCurrentDashboard();
    } catch (const std::exception& error) {
        PublishDashboardStatus("error", 0, 0, 0, error.what());
    }
    try {
        PublishCurrentSchedules();
    } catch (const std::exception& error) {
        PublishSchedulesStatus("error", 0, 0, 0, 0, error.what());
    }
    PublishBackgroundUploadStatus("idle");
}

std::optional<ManagerMqttService::IncomingCommand> ManagerMqttService::ParseIncoming(
    mdv::MqttMessage message) {
    auto makeCommand = [&](IncomingType type) {
        IncomingCommand command;
        command.type = type;
        command.message = std::move(message);
        return std::optional<IncomingCommand>(std::move(command));
    };

    if (message.topic == ConfigSetTopic) {
        return makeCommand(IncomingType::Configuration);
    }
    if (message.topic == DashboardConfigSetTopic) {
        return makeCommand(IncomingType::DashboardConfiguration);
    }
    if (message.topic == SchedulesConfigSetTopic) {
        return makeCommand(IncomingType::SchedulesConfiguration);
    }
    if (message.topic == BackgroundUploadStartTopic) {
        return makeCommand(IncomingType::BackgroundUploadStart);
    }

    auto parseUploadId = [&](std::string_view prefix, IncomingType type)
        -> std::optional<IncomingCommand> {
        if (message.topic.rfind(prefix, 0U) != 0U) {
            return std::nullopt;
        }
        const std::string_view uploadId(
            message.topic.data() + prefix.size(), message.topic.size() - prefix.size());
        if (!IsSafeUploadTopicId(uploadId) || uploadId.find('/') != std::string_view::npos) {
            return std::nullopt;
        }
        IncomingCommand command;
        command.type = type;
        command.uploadId = std::string(uploadId);
        command.message = std::move(message);
        return command;
    };

    if (message.topic.rfind(BackgroundChunkPrefix, 0U) == 0U) {
        const std::string_view remainder(
            message.topic.data() + BackgroundChunkPrefix.size(),
            message.topic.size() - BackgroundChunkPrefix.size());
        const std::size_t separator = remainder.find('/');
        if (separator == std::string_view::npos || separator == 0U ||
            remainder.find('/', separator + 1U) != std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view uploadId = remainder.substr(0U, separator);
        const std::string_view indexText = remainder.substr(separator + 1U);
        if (!IsSafeUploadTopicId(uploadId) || indexText.empty()) {
            return std::nullopt;
        }
        std::size_t index = 0;
        const auto parse = std::from_chars(
            indexText.data(), indexText.data() + indexText.size(), index);
        if (parse.ec != std::errc{} || parse.ptr != indexText.data() + indexText.size()) {
            return std::nullopt;
        }
        IncomingCommand command;
        command.type = IncomingType::BackgroundUploadChunk;
        command.uploadId = std::string(uploadId);
        command.chunkIndex = index;
        command.message = std::move(message);
        return command;
    }
    if (auto command = parseUploadId(
            BackgroundFinishPrefix, IncomingType::BackgroundUploadFinish)) {
        return command;
    }
    if (auto command = parseUploadId(
            BackgroundCancelPrefix, IncomingType::BackgroundUploadCancel)) {
        return command;
    }

    if (message.topic.rfind(ScheduleTopicPrefix, 0U) == 0U) {
        const std::string_view remainder(
            message.topic.data() + ScheduleTopicPrefix.size(),
            message.topic.size() - ScheduleTopicPrefix.size());
        constexpr std::string_view suffix = "/run";
        if (remainder.size() > suffix.size() &&
            remainder.ends_with(suffix)) {
            const std::string_view scheduleId =
                remainder.substr(0U, remainder.size() - suffix.size());
            if (IsSafeUploadTopicId(scheduleId) &&
                scheduleId.find('/') == std::string_view::npos) {
                IncomingCommand command;
                command.type = IncomingType::ScheduleRun;
                command.scheduleId = std::string(scheduleId);
                command.message = std::move(message);
                return command;
            }
        }
        return std::nullopt;
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
    IncomingCommand command;
    command.type = type;
    command.busId = busId;
    command.message = std::move(message);
    return command;
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
    if (auto completion = ProcessDiscoveryCompletion()) {
        return completion;
    }

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
    if (command.type == IncomingType::DashboardConfiguration) {
        return ProcessDashboardConfiguration(command.message);
    }
    if (command.type == IncomingType::SchedulesConfiguration) {
        return ProcessSchedulesConfiguration(command.message);
    }
    if (command.type == IncomingType::ScheduleRun) {
        return ProcessScheduleRun(command.scheduleId, command.message);
    }
    if (command.type == IncomingType::BackgroundUploadStart) {
        return ProcessBackgroundUploadStart(command.message);
    }
    if (command.type == IncomingType::BackgroundUploadChunk) {
        return ProcessBackgroundUploadChunk(
            command.uploadId, *command.chunkIndex, command.message);
    }
    if (command.type == IncomingType::BackgroundUploadFinish) {
        return ProcessBackgroundUploadFinish(command.uploadId, command.message);
    }
    if (command.type == IncomingType::BackgroundUploadCancel) {
        return ProcessBackgroundUploadCancel(command.uploadId, command.message);
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
            // Revision 0 can recover a missing or damaged buses.json.
        }

        BusesConfig config = ParseBusesConfig(message.payload);
        const int currentRevision =
            previousConfig.has_value() ? previousConfig->revision : 0;

        if (config.revision != currentRevision) {
            const std::string detail =
                "Configuration revision conflict: submitted " +
                std::to_string(config.revision) + ", current " +
                std::to_string(currentRevision);

            if (previousConfig.has_value()) {
                const std::string currentCanonical =
                    SerializeBusesConfig(*previousConfig);
                PublishResult(
                    false,
                    false,
                    detail,
                    previousConfig->buses.size(),
                    EnabledCount(*previousConfig));
                client_.Publish(ConfigTopic, currentCanonical, true);
                PublishReadyStatus(
                    previousConfig->buses.size(),
                    EnabledCount(*previousConfig));
            } else {
                PublishResult(false, false, detail);
            }
            return ManagerMqttResult{
                false, false, detail, std::nullopt, {}};
        }

        if (currentRevision == std::numeric_limits<int>::max()) {
            throw BusesConfigError(
                "configuration revision limit has been reached");
        }

        const DashboardCollection dashboards = LoadOrCreateDashboard();
        const SchedulesConfig schedules = LoadOrCreateSchedules();

        std::vector<DashboardReferenceIssue> currentDashboardIssues;
        std::vector<ScheduleReferenceIssue> currentScheduleIssues;
        if (previousConfig.has_value()) {
            currentDashboardIssues =
                InspectDashboardReferences(dashboards, *previousConfig);
            currentScheduleIssues =
                InspectScheduleReferences(
                    schedules, *previousConfig, dashboards);
        }

        const std::vector<DashboardReferenceIssue> submittedDashboardIssues =
            InspectDashboardReferences(dashboards, config);
        const std::vector<ScheduleReferenceIssue> submittedScheduleIssues =
            InspectScheduleReferences(schedules, config, dashboards);

        const std::size_t newDashboardIssues = CountNewReferenceIssues(
            currentDashboardIssues,
            submittedDashboardIssues,
            DashboardIssueKey);
        const std::size_t newScheduleIssues = CountNewReferenceIssues(
            currentScheduleIssues,
            submittedScheduleIssues,
            ScheduleIssueKey);

        if (newDashboardIssues != 0U || newScheduleIssues != 0U) {
            const std::string detail =
                "Configuration would break references: dashboard=" +
                std::to_string(newDashboardIssues) + ", schedules=" +
                std::to_string(newScheduleIssues);
            if (previousConfig.has_value()) {
                PublishResult(
                    false,
                    false,
                    detail,
                    previousConfig->buses.size(),
                    EnabledCount(*previousConfig));
                client_.Publish(
                    ConfigTopic,
                    SerializeBusesConfig(*previousConfig),
                    true);
                PublishReadyStatus(
                    previousConfig->buses.size(),
                    EnabledCount(*previousConfig));
            } else {
                PublishResult(false, false, detail);
            }
            return ManagerMqttResult{
                false, false, detail, std::nullopt, {}};
        }

        config.revision = currentRevision + 1;
        const std::string canonical = SerializeBusesConfig(config);
        const std::size_t enabledCount = EnabledCount(config);

        WriteTextFileAtomically(configPath_, canonical);
        client_.Publish(ConfigTopic, canonical, true);
        if (previousConfig.has_value()) {
            RemoveObsoleteDeviceTopics(*previousConfig, config);
        }
        try {
            PublishCurrentDashboard();
        } catch (const std::exception& error) {
            PublishDashboardStatus("error", 0, 0, 0, error.what());
        }
        try {
            PublishCurrentSchedules();
        } catch (const std::exception& error) {
            PublishSchedulesStatus("error", 0, 0, 0, 0, error.what());
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

ManagerMqttResult ManagerMqttService::ProcessDashboardConfiguration(
    const mdv::MqttMessage& message) {
    constexpr std::string_view command = "dashboard-config";
    if (message.retained) {
        constexpr std::string_view error =
            "Retained dashboard configuration commands are ignored";
        PublishDashboardResult(false, false, error, 0, 0, 0);
        return ManagerMqttResult{
            false, false, std::string(error), std::nullopt, std::string(command)};
    }
    if (message.payload.size() > MaximumDashboardPayloadBytes) {
        constexpr std::string_view error =
            "Dashboard configuration payload exceeds 1048576 bytes";
        PublishDashboardResult(false, false, error, 0, 0, 0);
        return ManagerMqttResult{
            false, false, std::string(error), std::nullopt, std::string(command)};
    }

    try {
        DashboardCollection current = LoadOrCreateDashboard();
        DashboardCollection submitted = ParseDashboardCollection(message.payload);
        if (submitted.revision != current.revision) {
            const std::string detail =
                "Dashboard revision conflict: submitted " +
                std::to_string(submitted.revision) + ", current " +
                std::to_string(current.revision);
            const BusesConfig buses = LoadBusesConfig(configPath_);
            const std::size_t issues =
                InspectDashboardReferences(current, buses).size();
            const std::string currentCanonical =
                SerializeDashboardCollection(current);
            PublishDashboardResult(
                false,
                false,
                detail,
                current.revision,
                std::accumulate(current.panels.begin(), current.panels.end(), std::size_t{0}, [](std::size_t total, const DashboardPanel& panel) { return total + panel.fans.size(); }),
                issues);
            // Publish the rejection first so web editors clear their pending-save
            // state before the retained server configuration arrives. Dirty
            // local drafts are then preserved while their base revision updates.
            client_.Publish(DashboardConfigTopic, currentCanonical, true);
            PublishDashboardStatus(
                "ready",
                current.revision,
                std::accumulate(current.panels.begin(), current.panels.end(), std::size_t{0}, [](std::size_t total, const DashboardPanel& panel) { return total + panel.fans.size(); }),
                issues,
                detail);
            return ManagerMqttResult{
                false, false, detail, std::nullopt, std::string(command)};
        }
        if (current.revision == std::numeric_limits<int>::max()) {
            throw DashboardConfigError("dashboard revision limit has been reached");
        }

        submitted.revision = current.revision + 1;
        const std::string canonical = SerializeDashboardCollection(submitted);
        const BusesConfig buses = LoadBusesConfig(configPath_);
        const std::size_t issues =
            InspectDashboardReferences(submitted, buses).size();

        WriteTextFileAtomically(dashboardPath_, canonical);
        client_.Publish(DashboardConfigTopic, canonical, true);
        PublishDashboardStatus(
            "ready",
            submitted.revision,
            std::accumulate(submitted.panels.begin(), submitted.panels.end(), std::size_t{0}, [](std::size_t total, const DashboardPanel& panel) { return total + panel.fans.size(); }),
            issues);
        PublishDashboardResult(
            true,
            true,
            "Dashboard configuration saved",
            submitted.revision,
            std::accumulate(submitted.panels.begin(), submitted.panels.end(), std::size_t{0}, [](std::size_t total, const DashboardPanel& panel) { return total + panel.fans.size(); }),
            issues);
        try {
            PublishCurrentSchedules();
        } catch (const std::exception& error) {
            PublishSchedulesStatus("error", 0, 0, 0, 0, error.what());
        }
        return ManagerMqttResult{
            true,
            true,
            "Dashboard configuration saved",
            std::nullopt,
            std::string(command)};
    } catch (const DashboardConfigError& error) {
        const std::string detail =
            std::string("Invalid dashboard configuration: ") + error.what();
        PublishDashboardResult(false, false, detail, 0, 0, 0);
        return ManagerMqttResult{
            false, false, detail, std::nullopt, std::string(command)};
    } catch (const std::exception& error) {
        const std::string detail =
            std::string("Cannot save dashboard configuration: ") + error.what();
        PublishDashboardStatus("error", 0, 0, 0, detail);
        PublishDashboardResult(false, false, detail, 0, 0, 0);
        return ManagerMqttResult{
            false, false, detail, std::nullopt, std::string(command)};
    }
}

ManagerMqttResult ManagerMqttService::ProcessSchedulesConfiguration(
    const mdv::MqttMessage& message) {
    constexpr std::string_view command = "schedules-config";
    if (message.retained) {
        constexpr std::string_view error =
            "Retained schedules configuration commands are ignored";
        PublishSchedulesResult(false, false, error, 0, 0, 0, 0);
        return ManagerMqttResult{
            false, false, std::string(error), std::nullopt, std::string(command)};
    }
    if (message.payload.size() > MaximumSchedulesPayloadBytes) {
        constexpr std::string_view error =
            "Schedules configuration payload exceeds 1048576 bytes";
        PublishSchedulesResult(false, false, error, 0, 0, 0, 0);
        return ManagerMqttResult{
            false, false, std::string(error), std::nullopt, std::string(command)};
    }

    try {
        SchedulesConfig current = LoadOrCreateSchedules();
        SchedulesConfig submitted = ParseSchedulesConfig(message.payload);
        const BusesConfig buses = LoadBusesConfig(configPath_);
        const DashboardCollection dashboards = LoadOrCreateDashboard();

        if (submitted.revision != current.revision) {
            const std::string detail =
                "Schedules revision conflict: submitted " +
                std::to_string(submitted.revision) + ", current " +
                std::to_string(current.revision);
            const std::size_t issues =
                InspectScheduleReferences(current, buses, dashboards).size();
            const std::string currentCanonical =
                SerializeSchedulesConfig(current);
            PublishSchedulesResult(
                false,
                false,
                detail,
                current.revision,
                current.schedules.size(),
                EnabledScheduleCount(current),
                issues);
            // Match the buses conflict contract: result first, current retained
            // configuration second. The web editor can keep its dirty draft and
            // retry it against the newly received revision without reloading.
            client_.Publish(SchedulesConfigTopic, currentCanonical, true);
            PublishSchedulesStatus(
                issues == 0U ? "ready" : "warning",
                current.revision,
                current.schedules.size(),
                EnabledScheduleCount(current),
                issues,
                detail);
            return ManagerMqttResult{
                false, false, detail, std::nullopt, std::string(command)};
        }
        if (current.revision == std::numeric_limits<int>::max()) {
            throw SchedulesConfigError("schedules revision limit has been reached");
        }

        ValidateScheduleReferences(submitted, buses, dashboards);
        submitted.revision = current.revision + 1;
        const std::string canonical = SerializeSchedulesConfig(submitted);
        WriteTextFileAtomically(schedulesPath_, canonical);
        client_.Publish(SchedulesConfigTopic, canonical, true);
        PublishSchedulesStatus(
            "ready",
            submitted.revision,
            submitted.schedules.size(),
            EnabledScheduleCount(submitted),
            0);
        PublishSchedulesResult(
            true,
            true,
            "Schedules configuration saved",
            submitted.revision,
            submitted.schedules.size(),
            EnabledScheduleCount(submitted),
            0);
        return ManagerMqttResult{
            true,
            true,
            "Schedules configuration saved",
            std::nullopt,
            std::string(command)};
    } catch (const SchedulesConfigError& error) {
        const std::string detail =
            std::string("Invalid schedules configuration: ") + error.what();
        PublishSchedulesResult(false, false, detail, 0, 0, 0, 0);
        return ManagerMqttResult{
            false, false, detail, std::nullopt, std::string(command)};
    } catch (const std::exception& error) {
        const std::string detail =
            std::string("Cannot save schedules configuration: ") + error.what();
        PublishSchedulesStatus("error", 0, 0, 0, 0, detail);
        PublishSchedulesResult(false, false, detail, 0, 0, 0, 0);
        return ManagerMqttResult{
            false, false, detail, std::nullopt, std::string(command)};
    }
}

ManagerMqttResult ManagerMqttService::ProcessScheduleRun(
    std::string_view scheduleId,
    const mdv::MqttMessage& message) {
    constexpr std::string_view command = "schedule-run";
    if (message.retained) {
        constexpr std::string_view detail = "Retained schedule run commands are ignored";
        PublishScheduleRunResult(scheduleId, false, "rejected", detail);
        return ManagerMqttResult{
            false, false, std::string(detail), std::nullopt, std::string(command)};
    }
    if (!message.payload.empty() && message.payload != "1" &&
        message.payload != "run" && message.payload != "{}") {
        constexpr std::string_view detail =
            "Schedule run payload must be empty, '1', 'run' or '{}'";
        PublishScheduleRunResult(scheduleId, false, "rejected", detail);
        return ManagerMqttResult{
            false, false, std::string(detail), std::nullopt, std::string(command)};
    }

    try {
        const SchedulesConfig schedules = LoadOrCreateSchedules();
        const ScheduleEntry* schedule = FindSchedule(schedules, scheduleId);
        if (schedule == nullptr) {
            throw SchedulesConfigError(
                "schedule '" + std::string(scheduleId) + "' does not exist");
        }
        SchedulesConfig selected;
        selected.revision = schedules.revision;
        selected.schedules.push_back(*schedule);
        ValidateScheduleReferences(
            selected,
            LoadBusesConfig(configPath_),
            LoadOrCreateDashboard());

        const std::string payload =
            "{\"version\":1,\"scheduleId\":\"" +
            JsonEscape(scheduleId) + "\",\"source\":\"manual\"}";
        const mdv::MqttPublishStatus publishStatus = client_.PublishWithResult(
            ScheduleExecuteTopic(scheduleId), payload, false);
        if (publishStatus != mdv::MqttPublishStatus::Published) {
            throw std::runtime_error(
                "cannot publish scheduler execute event: " +
                std::string(mdv::MqttPublishStatusMessage(publishStatus)));
        }
        PublishScheduleRunResult(
            scheduleId, true, "queued", "Schedule queued for execution");
        return ManagerMqttResult{
            true,
            false,
            "Schedule queued for execution",
            std::nullopt,
            std::string(command)};
    } catch (const std::exception& error) {
        const std::string detail =
            std::string("Cannot queue schedule: ") + error.what();
        PublishScheduleRunResult(scheduleId, false, "rejected", detail);
        return ManagerMqttResult{
            false, false, detail, std::nullopt, std::string(command)};
    }
}

ManagerMqttResult ManagerMqttService::ProcessBackgroundUploadStart(
    const mdv::MqttMessage& message) {
    constexpr std::string_view command = "background-upload-start";
    if (message.retained) {
        constexpr std::string_view detail = "Retained background upload commands are ignored";
        PublishBackgroundUploadResult(false, false, detail);
        return ManagerMqttResult{
            false, false, std::string(detail), std::nullopt, std::string(command)};
    }

    try {
        const DashboardUploadStartRequest request =
            ParseDashboardUploadStart(message.payload);
        const DashboardCollection dashboard = LoadOrCreateDashboard();
        if (FindDashboardPanel(dashboard, request.panelId) == nullptr) {
            throw DashboardUploadError("panelId does not reference an existing panel");
        }
        if (request.revision != dashboard.revision) {
            const std::string detail =
                "Dashboard revision conflict: submitted " +
                std::to_string(request.revision) + ", current " +
                std::to_string(dashboard.revision);
            PublishBackgroundUploadStatus(
                "error", request.uploadId, request.fileName, 0, request.size, detail);
            PublishBackgroundUploadResult(
                false, false, detail, request.uploadId, request.fileName,
                request.sha256, request.size, 0, 0, dashboard.revision);
            return ManagerMqttResult{
                false, false, detail, std::nullopt, std::string(command)};
        }

        backgroundUpload_.Start(request);
        PublishBackgroundUploadStatus(
            "uploading", request.uploadId, request.fileName, 0, request.size,
            "Upload started");
        PublishBackgroundUploadResult(
            true, false, "Upload started", request.uploadId, request.fileName,
            request.sha256, request.size, 0, 0, request.revision);
        return ManagerMqttResult{
            true, false, "Upload started", std::nullopt, std::string(command)};
    } catch (const std::exception& error) {
        const std::string detail =
            std::string("Cannot start background upload: ") + error.what();
        PublishBackgroundUploadStatus("error", {}, {}, 0, 0, detail);
        PublishBackgroundUploadResult(false, false, detail);
        return ManagerMqttResult{
            false, false, detail, std::nullopt, std::string(command)};
    }
}

ManagerMqttResult ManagerMqttService::ProcessBackgroundUploadChunk(
    std::string_view uploadId,
    std::size_t chunkIndex,
    const mdv::MqttMessage& message) {
    constexpr std::string_view command = "background-upload-chunk";
    if (message.retained) {
        constexpr std::string_view detail = "Retained background upload chunks are ignored";
        PublishBackgroundUploadResult(false, false, detail, uploadId);
        return ManagerMqttResult{
            false, false, std::string(detail), std::nullopt, std::string(command)};
    }

    try {
        backgroundUpload_.Append(uploadId, chunkIndex, message.payload);
        PublishBackgroundUploadStatus(
            "uploading",
            backgroundUpload_.UploadId(),
            backgroundUpload_.FileName(),
            backgroundUpload_.ReceivedBytes(),
            backgroundUpload_.ExpectedBytes());
        return ManagerMqttResult{
            true, false, "Upload chunk accepted", std::nullopt, std::string(command)};
    } catch (const std::exception& error) {
        const std::string detail =
            std::string("Cannot accept background upload chunk: ") + error.what();
        PublishBackgroundUploadStatus(
            "error",
            backgroundUpload_.UploadId(),
            backgroundUpload_.FileName(),
            backgroundUpload_.ReceivedBytes(),
            backgroundUpload_.ExpectedBytes(),
            detail);
        PublishBackgroundUploadResult(false, false, detail, uploadId);
        return ManagerMqttResult{
            false, false, detail, std::nullopt, std::string(command)};
    }
}

ManagerMqttResult ManagerMqttService::ProcessBackgroundUploadFinish(
    std::string_view uploadId,
    const mdv::MqttMessage& message) {
    constexpr std::string_view command = "background-upload-finish";
    if (message.retained) {
        constexpr std::string_view detail = "Retained background upload commands are ignored";
        PublishBackgroundUploadResult(false, false, detail, uploadId);
        return ManagerMqttResult{
            false, false, std::string(detail), std::nullopt, std::string(command)};
    }

    std::filesystem::path committedPath;
    bool createdAsset = false;
    try {
        DashboardCollection dashboard = LoadOrCreateDashboard();
        if (!backgroundUpload_.Active() || uploadId != backgroundUpload_.UploadId()) {
            throw DashboardUploadError("uploadId does not match the active upload");
        }
        if (dashboard.revision != backgroundUpload_.ExpectedRevision()) {
            const std::string detail =
                "Dashboard revision conflict: upload started at " +
                std::to_string(backgroundUpload_.ExpectedRevision()) + ", current " +
                std::to_string(dashboard.revision);
            backgroundUpload_.Cancel(uploadId);
            PublishBackgroundUploadStatus("error", uploadId, {}, 0, 0, detail);
            PublishBackgroundUploadResult(
                false, false, detail, uploadId, {}, {}, 0, 0, 0, dashboard.revision);
            return ManagerMqttResult{
                false, false, detail, std::nullopt, std::string(command)};
        }
        if (dashboard.revision == std::numeric_limits<int>::max()) {
            throw DashboardUploadError("dashboard revision limit has been reached");
        }

        const DashboardPreparedAsset asset = backgroundUpload_.Prepare(uploadId);
        std::error_code filesystemError;
        if (std::filesystem::exists(asset.finalPath, filesystemError)) {
            if (filesystemError) {
                throw std::runtime_error(
                    "cannot inspect dashboard asset: " + filesystemError.message());
            }
            if (ComputeFileSha256(asset.finalPath) != asset.sha256) {
                throw std::runtime_error(
                    "existing dashboard asset has the same name but different content");
            }
            std::filesystem::remove(asset.temporaryPath, filesystemError);
            if (filesystemError) {
                throw std::runtime_error(
                    "cannot remove duplicate temporary asset: " + filesystemError.message());
            }
        } else {
            if (filesystemError) {
                throw std::runtime_error(
                    "cannot inspect dashboard asset: " + filesystemError.message());
            }
            std::filesystem::rename(asset.temporaryPath, asset.finalPath, filesystemError);
            if (filesystemError) {
                throw std::runtime_error(
                    "cannot commit dashboard asset: " + filesystemError.message());
            }
            committedPath = asset.finalPath;
            createdAsset = true;
        }

        DashboardPanel* panel = FindDashboardPanel(dashboard, asset.panelId);
        if (panel == nullptr) {
            throw DashboardUploadError("panelId does not reference an existing panel");
        }
        const std::string previousFile = panel->background.file;
        panel->background.file = asset.finalFileName;
        panel->background.naturalWidth = asset.width;
        panel->background.naturalHeight = asset.height;
        ++dashboard.revision;
        const std::string canonical = SerializeDashboardCollection(dashboard);
        const BusesConfig buses = LoadBusesConfig(configPath_);
        const std::size_t issues = InspectDashboardReferences(dashboard, buses).size();

        try {
            WriteTextFileAtomically(dashboardPath_, canonical);
        } catch (...) {
            if (createdAsset) {
                std::filesystem::remove(committedPath, filesystemError);
            }
            throw;
        }

        backgroundUpload_.Release();
        client_.Publish(DashboardConfigTopic, canonical, true);
        PublishDashboardStatus(
            "ready", dashboard.revision, std::accumulate(dashboard.panels.begin(), dashboard.panels.end(), std::size_t{0}, [](std::size_t total, const DashboardPanel& panel) { return total + panel.fans.size(); }), issues);
        PublishDashboardResult(
            true, true, "Dashboard background updated",
            dashboard.revision, std::accumulate(dashboard.panels.begin(), dashboard.panels.end(), std::size_t{0}, [](std::size_t total, const DashboardPanel& panel) { return total + panel.fans.size(); }), issues);
        PublishBackgroundUploadStatus(
            "completed", asset.uploadId, asset.finalFileName,
            asset.size, asset.size, "Background image uploaded");
        PublishBackgroundUploadResult(
            true, true, "Background image uploaded",
            asset.uploadId, asset.finalFileName, asset.sha256,
            asset.size, asset.width, asset.height, dashboard.revision);

        const bool previousStillUsed = std::any_of(
            dashboard.panels.begin(), dashboard.panels.end(),
            [&](const DashboardPanel& item) { return item.background.file == previousFile; });
        if (!previousFile.empty() && previousFile != asset.finalFileName &&
            !previousStillUsed && IsManagedBackgroundFile(previousFile)) {
            std::filesystem::remove(
                backgroundUpload_.AssetDirectory() / previousFile, filesystemError);
        }
        return ManagerMqttResult{
            true, true, "Background image uploaded",
            std::nullopt, std::string(command)};
    } catch (const std::exception& error) {
        if (createdAsset && !committedPath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(committedPath, ignored);
        }
        try {
            backgroundUpload_.Cancel(uploadId);
        } catch (...) {
        }
        const std::string detail =
            std::string("Cannot finish background upload: ") + error.what();
        PublishBackgroundUploadStatus("error", uploadId, {}, 0, 0, detail);
        PublishBackgroundUploadResult(false, false, detail, uploadId);
        return ManagerMqttResult{
            false, false, detail, std::nullopt, std::string(command)};
    }
}

ManagerMqttResult ManagerMqttService::ProcessBackgroundUploadCancel(
    std::string_view uploadId,
    const mdv::MqttMessage& message) {
    constexpr std::string_view command = "background-upload-cancel";
    if (message.retained) {
        constexpr std::string_view detail = "Retained background upload commands are ignored";
        PublishBackgroundUploadResult(false, false, detail, uploadId);
        return ManagerMqttResult{
            false, false, std::string(detail), std::nullopt, std::string(command)};
    }

    try {
        backgroundUpload_.Cancel(uploadId);
        PublishBackgroundUploadStatus("idle");
        PublishBackgroundUploadResult(
            true, false, "Background upload cancelled", uploadId);
        return ManagerMqttResult{
            true, false, "Background upload cancelled",
            std::nullopt, std::string(command)};
    } catch (const std::exception& error) {
        const std::string detail =
            std::string("Cannot cancel background upload: ") + error.what();
        PublishBackgroundUploadResult(false, false, detail, uploadId);
        return ManagerMqttResult{
            false, false, detail, std::nullopt, std::string(command)};
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
        case IncomingType::DashboardConfiguration:
            throw std::logic_error("dashboard configuration command routed as bus command");
        case IncomingType::SchedulesConfiguration:
        case IncomingType::ScheduleRun:
            throw std::logic_error("schedule command routed as bus command");
        case IncomingType::BackgroundUploadStart:
        case IncomingType::BackgroundUploadChunk:
        case IncomingType::BackgroundUploadFinish:
        case IncomingType::BackgroundUploadCancel:
            throw std::logic_error("background upload command routed as bus command");
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
        return ManagerMqttResult{
            false, false, detail, busId, std::string(command)};
    }
    if (discoveryRunner_ == nullptr) {
        const std::string detail = "Discovery runner is not configured";
        PublishBusResult(busId, command, false, detail);
        PublishDiscoveryStatus(busId, "error", {}, detail);
        return ManagerMqttResult{
            false, false, detail, busId, std::string(command)};
    }
    if (DiscoveryBusy()) {
        const std::string detail =
            "Another discovery is already running";
        PublishDiscoveryResult(busId, false, {}, detail);
        PublishBusResult(busId, command, false, detail);
        return ManagerMqttResult{
            false, false, detail, busId, std::string(command)};
    }

    try {
        const BusesConfig config = LoadBusesConfig(configPath_);
        const BusConfig* bus = FindBus(config, busId);
        if (bus == nullptr) {
            const std::string detail = "Bus is not configured";
            PublishBusResult(busId, command, false, detail);
            PublishDiscoveryStatus(busId, "error", {}, detail);
            return ManagerMqttResult{
                false, false, detail, busId, std::string(command)};
        }

        PublishDiscoveryStatus(
            busId,
            "running",
            bus->port,
            "Discovery is running");
        const BusServiceStatus serviceStatus =
            QueryBusServiceStatus(
                busId,
                servicePaths_,
                commandRunner_);
        if (serviceStatus.active) {
            ExecuteBusServiceCommand(
                busId,
                BusServiceCommand::Stop,
                servicePaths_,
                commandRunner_);
        }
        PublishBusStatus(*bus);

        if (!StartDiscoveryWorker(busId, bus->port)) {
            const std::string detail =
                "Another discovery is already running";
            PublishDiscoveryResult(busId, false, {}, detail);
            PublishBusResult(busId, command, false, detail);
            return ManagerMqttResult{
                false, false, detail, busId, std::string(command)};
        }

        // Keep existing fast unit-test runners deterministic without allowing a
        // real 29-second serial scan to block the manager loop.
        if (WaitForDiscoveryCompletion(DiscoveryInlineCompletionWait)) {
            if (auto completion = ProcessDiscoveryCompletion()) {
                return *completion;
            }
        }

        const std::string detail =
            "Discovery started in background";
        PublishBusResult(busId, command, true, detail);
        return ManagerMqttResult{
            true, false, detail, busId, std::string(command)};
    } catch (const std::exception& error) {
        const std::string detail =
            std::string("Discovery failed: ") + error.what();
        PublishDiscoveryResult(busId, false, {}, detail);
        PublishDiscoveryStatus(busId, "error", {}, detail);
        PublishBusResult(busId, command, false, detail);
        return ManagerMqttResult{
            false, false, detail, busId, std::string(command)};
    }
}

bool ManagerMqttService::StartDiscoveryWorker(
    int busId,
    std::string port) {
    {
        std::lock_guard lock(discoveryMutex_);
        if (discoveryRunning_ ||
            discoveryCompletion_.has_value() ||
            discoveryThread_.joinable()) {
            return false;
        }
        discoveryRunning_ = true;
    }

    try {
        discoveryThread_ = std::thread(
            [this, busId, port = std::move(port)]() mutable {
                DiscoveryExecutionResult result;
                try {
                    constexpr int MasterId = 0;
                    constexpr int PeriodMilliseconds = 150;
                    constexpr int ResponseTimeoutMilliseconds = 130;
                    result = discoveryRunner_->Run(
                        port,
                        MasterId,
                        PeriodMilliseconds,
                        ResponseTimeoutMilliseconds);
                } catch (const std::exception& error) {
                    result.success = false;
                    result.message =
                        std::string("Discovery failed: ") +
                        error.what();
                } catch (...) {
                    result.success = false;
                    result.message =
                        "Discovery failed with an unknown error";
                }

                {
                    std::lock_guard lock(discoveryMutex_);
                    discoveryCompletion_ = DiscoveryCompletion{
                        busId,
                        std::move(port),
                        std::move(result)};
                    discoveryRunning_ = false;
                }
                discoveryCondition_.notify_all();
            });
    } catch (...) {
        std::lock_guard lock(discoveryMutex_);
        discoveryRunning_ = false;
        throw;
    }
    return true;
}

bool ManagerMqttService::WaitForDiscoveryCompletion(
    std::chrono::milliseconds timeout) {
    std::unique_lock lock(discoveryMutex_);
    return discoveryCondition_.wait_for(
        lock,
        timeout,
        [this] {
            return discoveryCompletion_.has_value();
        });
}

bool ManagerMqttService::DiscoveryBusy() const {
    std::lock_guard lock(discoveryMutex_);
    return discoveryRunning_ ||
        discoveryCompletion_.has_value() ||
        discoveryThread_.joinable();
}

std::optional<ManagerMqttResult>
ManagerMqttService::ProcessDiscoveryCompletion() {
    std::optional<DiscoveryCompletion> completion;
    {
        std::lock_guard lock(discoveryMutex_);
        if (!discoveryCompletion_.has_value()) {
            return std::nullopt;
        }
        completion = std::move(discoveryCompletion_);
        discoveryCompletion_.reset();
    }

    if (discoveryThread_.joinable()) {
        discoveryThread_.join();
    }

    constexpr std::string_view command = "discovery";
    const DiscoveryExecutionResult& discovery =
        completion->result;
    if (!discovery.success) {
        const std::string detail = discovery.message.empty()
            ? "Discovery failed"
            : discovery.message;
        PublishDiscoveryResult(
            completion->busId,
            false,
            {},
            detail);
        PublishDiscoveryStatus(
            completion->busId,
            "error",
            completion->port,
            detail);
        PublishBusResult(
            completion->busId,
            command,
            false,
            detail);
        return ManagerMqttResult{
            false,
            false,
            detail,
            completion->busId,
            std::string(command)};
    }

    const std::string detail = discovery.addresses.empty()
        ? "Discovery completed; no devices found"
        : "Discovery completed";
    PublishDiscoveryResult(
        completion->busId,
        true,
        discovery.addresses,
        detail);
    PublishDiscoveryStatus(
        completion->busId,
        "completed",
        completion->port,
        detail,
        discovery.addresses.size());
    PublishBusResult(
        completion->busId,
        command,
        true,
        detail);
    return ManagerMqttResult{
        true,
        false,
        detail,
        completion->busId,
        std::string(command)};
}

void ManagerMqttService::JoinDiscoveryWorker() noexcept {
    if (discoveryThread_.joinable()) {
        discoveryThread_.join();
    }
}

std::size_t ManagerMqttService::PendingCount() const {
    std::size_t count = 0;
    {
        std::lock_guard lock(mutex_);
        count = inbox_.size();
    }
    {
        std::lock_guard lock(discoveryMutex_);
        if (discoveryCompletion_.has_value()) {
            ++count;
        }
    }
    return count;
}

void ManagerMqttService::PublishCurrentConfig() {
    const BusesConfig config = LoadBusesConfig(configPath_);
    client_.Publish(ConfigTopic, SerializeBusesConfig(config), true);
    PublishReadyStatus(config.buses.size(), EnabledCount(config));
    PublishAllBusStatuses(config);
    PublishDiscoveryIdleStatuses(config);
}

DashboardCollection ManagerMqttService::LoadOrCreateDashboard() {
    std::error_code error;
    if (std::filesystem::exists(dashboardPath_, error)) {
        if (error) {
            throw std::runtime_error(
                "cannot inspect dashboard configuration: " + error.message());
        }
        return LoadDashboardCollection(dashboardPath_);
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect dashboard configuration: " + error.message());
    }

    DashboardCollection dashboard;
    const std::string canonical = SerializeDashboardCollection(dashboard);
    WriteTextFileAtomically(dashboardPath_, canonical);
    return dashboard;
}

void ManagerMqttService::PublishCurrentDashboard() {
    const DashboardCollection dashboard = LoadOrCreateDashboard();
    const BusesConfig buses = LoadBusesConfig(configPath_);
    const std::size_t issues =
        InspectDashboardReferences(dashboard, buses).size();
    client_.Publish(
        DashboardConfigTopic,
        SerializeDashboardCollection(dashboard),
        true);
    PublishDashboardStatus(
        "ready",
        dashboard.revision,
        std::accumulate(dashboard.panels.begin(), dashboard.panels.end(), std::size_t{0}, [](std::size_t total, const DashboardPanel& panel) { return total + panel.fans.size(); }),
        issues);
}

SchedulesConfig ManagerMqttService::LoadOrCreateSchedules() {
    std::error_code error;
    if (std::filesystem::exists(schedulesPath_, error)) {
        if (error) {
            throw std::runtime_error(
                "cannot inspect schedules configuration: " + error.message());
        }
        return LoadSchedulesConfig(schedulesPath_);
    }
    if (error) {
        throw std::runtime_error(
            "cannot inspect schedules configuration: " + error.message());
    }

    SchedulesConfig schedules;
    const std::string canonical = SerializeSchedulesConfig(schedules);
    WriteTextFileAtomically(schedulesPath_, canonical);
    return schedules;
}

void ManagerMqttService::PublishCurrentSchedules() {
    const SchedulesConfig schedules = LoadOrCreateSchedules();
    const BusesConfig buses = LoadBusesConfig(configPath_);
    const DashboardCollection dashboards = LoadOrCreateDashboard();
    const std::size_t issues =
        InspectScheduleReferences(schedules, buses, dashboards).size();
    client_.Publish(
        SchedulesConfigTopic,
        SerializeSchedulesConfig(schedules),
        true);
    PublishSchedulesStatus(
        issues == 0U ? "ready" : "warning",
        schedules.revision,
        schedules.schedules.size(),
        EnabledScheduleCount(schedules),
        issues,
        issues == 0U ? std::string_view{} :
            std::string_view("Schedule references require attention"));
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

void ManagerMqttService::PublishDashboardStatus(
    std::string_view state,
    int revision,
    std::size_t fanCount,
    std::size_t referenceIssueCount,
    std::string_view message) {
    std::string payload =
        "{\"state\":\"" + JsonEscape(state) + "\"" +
        ",\"revision\":" + std::to_string(revision) +
        ",\"fans\":" + std::to_string(fanCount) +
        ",\"referenceIssues\":" + std::to_string(referenceIssueCount);
    if (!message.empty()) {
        payload += ",\"message\":\"" + JsonEscape(message) + "\"";
    }
    payload += '}';
    client_.Publish(DashboardStatusTopic, payload, true);
}

void ManagerMqttService::PublishDashboardResult(
    bool success,
    bool saved,
    std::string_view message,
    int revision,
    std::size_t fanCount,
    std::size_t referenceIssueCount) {
    const std::string payload =
        std::string("{\"success\":") + (success ? "true" : "false") +
        ",\"saved\":" + (saved ? "true" : "false") +
        ",\"message\":\"" + JsonEscape(message) + "\"" +
        ",\"revision\":" + std::to_string(revision) +
        ",\"fans\":" + std::to_string(fanCount) +
        ",\"referenceIssues\":" + std::to_string(referenceIssueCount) + "}";
    client_.Publish(DashboardConfigResultTopic, payload, false);
}

void ManagerMqttService::PublishSchedulesStatus(
    std::string_view state,
    int revision,
    std::size_t scheduleCount,
    std::size_t enabledCount,
    std::size_t referenceIssueCount,
    std::string_view message) {
    std::string payload =
        "{\"state\":\"" + JsonEscape(state) + "\"" +
        ",\"revision\":" + std::to_string(revision) +
        ",\"schedules\":" + std::to_string(scheduleCount) +
        ",\"enabled\":" + std::to_string(enabledCount) +
        ",\"referenceIssues\":" + std::to_string(referenceIssueCount);
    if (!message.empty()) {
        payload += ",\"message\":\"" + JsonEscape(message) + "\"";
    }
    payload += '}';
    client_.Publish(SchedulesStatusTopic, payload, true);
}

void ManagerMqttService::PublishSchedulesResult(
    bool success,
    bool saved,
    std::string_view message,
    int revision,
    std::size_t scheduleCount,
    std::size_t enabledCount,
    std::size_t referenceIssueCount) {
    const std::string payload =
        std::string("{\"success\":") + (success ? "true" : "false") +
        ",\"saved\":" + (saved ? std::string("true") : std::string("false")) +
        ",\"message\":\"" + JsonEscape(message) + "\"" +
        ",\"revision\":" + std::to_string(revision) +
        ",\"schedules\":" + std::to_string(scheduleCount) +
        ",\"enabled\":" + std::to_string(enabledCount) +
        ",\"referenceIssues\":" + std::to_string(referenceIssueCount) + "}";
    client_.Publish(SchedulesConfigResultTopic, payload, false);
}

void ManagerMqttService::PublishScheduleRunResult(
    std::string_view scheduleId,
    bool success,
    std::string_view state,
    std::string_view message) {
    const std::string payload =
        std::string("{\"success\":") + (success ? "true" : "false") +
        ",\"scheduleId\":\"" + JsonEscape(scheduleId) + "\"" +
        ",\"state\":\"" + JsonEscape(state) + "\"" +
        ",\"message\":\"" + JsonEscape(message) + "\"}";
    client_.Publish(ScheduleResultTopic(scheduleId), payload, false);
}

void ManagerMqttService::PublishBackgroundUploadStatus(
    std::string_view state,
    std::string_view uploadId,
    std::string_view fileName,
    std::size_t receivedBytes,
    std::size_t totalBytes,
    std::string_view message) {
    std::string payload = "{\"state\":\"" + JsonEscape(state) + "\"";
    if (!uploadId.empty()) {
        payload += ",\"uploadId\":\"" + JsonEscape(uploadId) + "\"";
    }
    if (!fileName.empty()) {
        payload += ",\"fileName\":\"" + JsonEscape(fileName) + "\"";
    }
    if (totalBytes != 0U) {
        payload += ",\"received\":" + std::to_string(receivedBytes) +
            ",\"total\":" + std::to_string(totalBytes) +
            ",\"progress\":" + std::to_string(
                static_cast<unsigned int>((receivedBytes * 100U) / totalBytes));
    }
    if (!message.empty()) {
        payload += ",\"message\":\"" + JsonEscape(message) + "\"";
    }
    payload += '}';
    client_.Publish(BackgroundUploadStatusTopic, payload, true);
}

void ManagerMqttService::PublishBackgroundUploadResult(
    bool success,
    bool saved,
    std::string_view message,
    std::string_view uploadId,
    std::string_view fileName,
    std::string_view sha256,
    std::size_t size,
    int width,
    int height,
    int revision) {
    std::string payload =
        std::string("{\"success\":") + (success ? "true" : "false") +
        ",\"saved\":" + (saved ? "true" : "false") +
        ",\"message\":\"" + JsonEscape(message) + "\"";
    if (!uploadId.empty()) {
        payload += ",\"uploadId\":\"" + JsonEscape(uploadId) + "\"";
    }
    if (!fileName.empty()) {
        payload += ",\"fileName\":\"" + JsonEscape(fileName) + "\"";
    }
    if (!sha256.empty()) {
        payload += ",\"sha256\":\"" + JsonEscape(sha256) + "\"";
    }
    if (size != 0U) {
        payload += ",\"size\":" + std::to_string(size);
    }
    if (width != 0 && height != 0) {
        payload += ",\"width\":" + std::to_string(width) +
            ",\"height\":" + std::to_string(height);
    }
    if (revision != 0) {
        payload += ",\"revision\":" + std::to_string(revision);
    }
    payload += '}';
    client_.Publish(BackgroundUploadResultTopic, payload, false);
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
            ReadStringEnvironment(
                "MDVWB_BINARY",
                "/usr/local/bin/MDVWB"),
            std::chrono::milliseconds(
                ReadIntegerEnvironment(
                    "MDVWB_DISCOVERY_TIMEOUT_MS",
                    45000)));
        const std::filesystem::path dashboardPath = ReadStringEnvironment(
            "MDVWB_DASHBOARD_CONFIG",
            (configPath.parent_path() / "dashboard.json").string());
        const std::filesystem::path dashboardAssetDirectory = ReadStringEnvironment(
            "MDVWB_DASHBOARD_ASSET_DIR",
            "/var/www/fancoils/assets");
        const std::filesystem::path schedulesPath = ReadStringEnvironment(
            "MDVWB_SCHEDULES_CONFIG",
            (configPath.parent_path() / "schedules.json").string());
        ManagerMqttService service(
            client,
            configPath,
            servicePaths,
            commandRunner,
            &discoveryRunner,
            dashboardPath,
            dashboardAssetDirectory,
            schedulesPath);

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
