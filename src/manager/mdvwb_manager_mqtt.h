#pragma once

#include "mdv_dashboard_config.h"
#include "mdv_schedules_config.h"
#include "mdvwb_dashboard_upload.h"
#include "mdv_mqtt.h"
#include "mdvwb_discovery_runner.h"
#include "mdvwb_service_sync.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace mdvwb {

struct ManagerMqttResult {
    bool success = false;
    bool saved = false;
    std::string message;
    std::optional<int> busId;
    std::string command;
};

class ManagerMqttService final {
public:
    static constexpr const char* ConfigTopic = "/mdvwb/config";
    static constexpr const char* ConfigSetTopic = "/mdvwb/config/set";
    static constexpr const char* ConfigResultTopic = "/mdvwb/config/result";
    static constexpr const char* StatusTopic = "/mdvwb/status";
    static constexpr const char* DashboardConfigTopic =
        "/mdvwb/dashboard/config";
    static constexpr const char* DashboardConfigSetTopic =
        "/mdvwb/dashboard/config/set";
    static constexpr const char* DashboardConfigResultTopic =
        "/mdvwb/dashboard/config/result";
    static constexpr const char* DashboardStatusTopic =
        "/mdvwb/dashboard/status";
    static constexpr const char* SchedulesConfigTopic =
        "/mdvwb/schedules/config";
    static constexpr const char* SchedulesConfigSetTopic =
        "/mdvwb/schedules/config/set";
    static constexpr const char* SchedulesConfigResultTopic =
        "/mdvwb/schedules/config/result";
    static constexpr const char* SchedulesStatusTopic =
        "/mdvwb/schedules/status";
    static constexpr const char* ScheduleRunFilter =
        "/mdvwb/schedules/+/run";
    static constexpr const char* BackgroundUploadStartTopic =
        "/mdvwb/dashboard/background/upload/start";
    static constexpr const char* BackgroundUploadChunkFilter =
        "/mdvwb/dashboard/background/upload/chunk/+/+";
    static constexpr const char* BackgroundUploadFinishFilter =
        "/mdvwb/dashboard/background/upload/finish/+";
    static constexpr const char* BackgroundUploadCancelFilter =
        "/mdvwb/dashboard/background/upload/cancel/+";
    static constexpr const char* BackgroundUploadStatusTopic =
        "/mdvwb/dashboard/background/upload/status";
    static constexpr const char* BackgroundUploadResultTopic =
        "/mdvwb/dashboard/background/upload/result";
    static constexpr const char* BusStartFilter = "/mdvwb/buses/+/start";
    static constexpr const char* BusStopFilter = "/mdvwb/buses/+/stop";
    static constexpr const char* BusRestartFilter =
        "/mdvwb/buses/+/restart";
    static constexpr const char* BusStatusGetFilter =
        "/mdvwb/buses/+/status/get";
    static constexpr const char* BusDiscoveryStartFilter =
        "/mdvwb/buses/+/discovery/start";

    static constexpr std::size_t MaximumPendingCommands = 128U;
    static constexpr std::size_t MaximumPendingBytes = 8U * 1024U * 1024U;

    ManagerMqttService(
        mdv::IMqttClient& client,
        std::filesystem::path configPath,
        ServiceSyncPaths servicePaths,
        CommandRunner& commandRunner,
        DiscoveryRunner* discoveryRunner = nullptr,
        std::filesystem::path dashboardPath = {},
        std::filesystem::path dashboardAssetDirectory = {},
        std::filesystem::path schedulesPath = {});
    ~ManagerMqttService();

    ManagerMqttService(const ManagerMqttService&) = delete;
    ManagerMqttService& operator=(const ManagerMqttService&) = delete;

    void Start();
    [[nodiscard]] std::optional<ManagerMqttResult> ProcessOne();
    [[nodiscard]] std::size_t PendingCount() const;

private:
    enum class IncomingType {
        Configuration,
        DashboardConfiguration,
        SchedulesConfiguration,
        ScheduleRun,
        BackgroundUploadStart,
        BackgroundUploadChunk,
        BackgroundUploadFinish,
        BackgroundUploadCancel,
        BusStart,
        BusStop,
        BusRestart,
        BusStatus,
        BusDiscovery,
    };

    struct IncomingCommand {
        IncomingType type = IncomingType::Configuration;
        std::optional<int> busId;
        std::string uploadId;
        std::string scheduleId;
        std::optional<std::size_t> chunkIndex;
        mdv::MqttMessage message;

        [[nodiscard]] bool HasSameQueueKey(
            const IncomingCommand& other) const noexcept
        {
            const auto isBusLifecycle = [](IncomingType value) {
                return value == IncomingType::BusStart ||
                    value == IncomingType::BusStop ||
                    value == IncomingType::BusRestart;
            };
            if (isBusLifecycle(type) && isBusLifecycle(other.type)) {
                return busId == other.busId;
            }

            const auto isUploadTerminal = [](IncomingType value) {
                return value == IncomingType::BackgroundUploadFinish ||
                    value == IncomingType::BackgroundUploadCancel;
            };
            if (isUploadTerminal(type) && isUploadTerminal(other.type)) {
                return uploadId == other.uploadId;
            }

            return message.topic == other.message.topic;
        }

        friend bool SameQueueKey(
            const IncomingCommand& left,
            const IncomingCommand& right) noexcept
        {
            return left.HasSameQueueKey(right);
        }

        friend std::size_t QueueItemBytes(
            const IncomingCommand& command) noexcept
        {
            return sizeof(IncomingCommand) + command.uploadId.size() +
                command.scheduleId.size() + command.message.topic.size() +
                command.message.payload.size();
        }
    };

    struct DiscoveryCompletion {
        int busId = 0;
        std::string port;
        DiscoveryExecutionResult result;
    };

    void Enqueue(mdv::MqttMessage message);
    [[nodiscard]] static std::optional<IncomingCommand> ParseIncoming(
        mdv::MqttMessage message);
    [[nodiscard]] ManagerMqttResult ProcessConfiguration(
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessDashboardConfiguration(
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessSchedulesConfiguration(
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessScheduleRun(
        std::string_view scheduleId,
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessBackgroundUploadStart(
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessBackgroundUploadChunk(
        std::string_view uploadId,
        std::size_t chunkIndex,
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessBackgroundUploadFinish(
        std::string_view uploadId,
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessBackgroundUploadCancel(
        std::string_view uploadId,
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessBusCommand(
        IncomingType type,
        int busId,
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessDiscovery(
        int busId,
        const mdv::MqttMessage& message);
    [[nodiscard]] std::optional<ManagerMqttResult>
        ProcessDiscoveryCompletion();
    [[nodiscard]] bool StartDiscoveryWorker(
        int busId,
        std::string port);
    [[nodiscard]] bool WaitForDiscoveryCompletion(
        std::chrono::milliseconds timeout);
    [[nodiscard]] bool DiscoveryBusy() const;
    void JoinDiscoveryWorker() noexcept;

    void PublishCurrentConfig();
    void PublishCurrentDashboard();
    [[nodiscard]] DashboardCollection LoadOrCreateDashboard();
    void PublishCurrentSchedules();
    [[nodiscard]] SchedulesConfig LoadOrCreateSchedules();
    void PublishReadyStatus(
        std::size_t busCount,
        std::size_t enabledCount);
    void PublishErrorStatus(std::string_view message);
    void PublishDashboardStatus(
        std::string_view state,
        int revision,
        std::size_t fanCount,
        std::size_t referenceIssueCount,
        std::string_view message = {});
    void PublishDashboardResult(
        bool success,
        bool saved,
        std::string_view message,
        int revision,
        std::size_t fanCount,
        std::size_t referenceIssueCount);
    void PublishSchedulesStatus(
        std::string_view state,
        int revision,
        std::size_t scheduleCount,
        std::size_t enabledCount,
        std::size_t referenceIssueCount,
        std::string_view message = {});
    void PublishSchedulesResult(
        bool success,
        bool saved,
        std::string_view message,
        int revision,
        std::size_t scheduleCount,
        std::size_t enabledCount,
        std::size_t referenceIssueCount);
    void PublishScheduleRunResult(
        std::string_view scheduleId,
        bool success,
        std::string_view state,
        std::string_view message);
    void PublishBackgroundUploadStatus(
        std::string_view state,
        std::string_view uploadId = {},
        std::string_view fileName = {},
        std::size_t receivedBytes = 0,
        std::size_t totalBytes = 0,
        std::string_view message = {});
    void PublishBackgroundUploadResult(
        bool success,
        bool saved,
        std::string_view message,
        std::string_view uploadId = {},
        std::string_view fileName = {},
        std::string_view sha256 = {},
        std::size_t size = 0,
        int width = 0,
        int height = 0,
        int revision = 0);
    void PublishResult(
        bool success,
        bool saved,
        std::string_view message,
        std::size_t busCount = 0,
        std::size_t enabledCount = 0,
        std::size_t actionCount = 0);
    void PublishAllBusStatuses(const BusesConfig& config);
    void PublishBusStatus(const BusConfig& bus);
    void PublishBusResult(
        int busId,
        std::string_view command,
        bool success,
        std::string_view message);
    void PublishDiscoveryStatus(
        int busId,
        std::string_view state,
        std::string_view port,
        std::string_view message = {},
        std::size_t foundCount = 0);
    void PublishDiscoveryResult(
        int busId,
        bool success,
        const std::vector<int>& addresses,
        std::string_view message);
    void PublishDiscoveryIdleStatuses(const BusesConfig& config);
    void RemoveObsoleteDeviceTopics(
        const BusesConfig& previous,
        const BusesConfig& current);
    void ClearFanDeviceTopics(int busId, int address);
    void ClearSystemDeviceTopics(int busId);
    void ClearRetained(std::string topic);

    mdv::IMqttClient& client_;
    std::filesystem::path configPath_;
    std::filesystem::path dashboardPath_;
    std::filesystem::path schedulesPath_;
    DashboardBackgroundUpload backgroundUpload_;
    ServiceSyncPaths servicePaths_;
    CommandRunner& commandRunner_;
    DiscoveryRunner* discoveryRunner_ = nullptr;

    mutable std::mutex mutex_;
    mdv::BoundedLatestQueue<
        IncomingCommand, MaximumPendingCommands, MaximumPendingBytes> inbox_;

    mutable std::mutex discoveryMutex_;
    std::condition_variable discoveryCondition_;
    std::thread discoveryThread_;
    bool discoveryRunning_ = false;
    std::optional<DiscoveryCompletion> discoveryCompletion_;

    bool started_ = false;
};

int RunManagerMqttDaemon(
    const std::filesystem::path& configPath,
    std::ostream& output,
    std::ostream& errors);

}  // namespace mdvwb
