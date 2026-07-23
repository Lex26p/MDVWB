#pragma once

#include "mdv_mqtt.h"
#include "mdvwb_discovery_runner.h"
#include "mdvwb_service_sync.h"

#include <cstddef>
#include <deque>
#include <filesystem>
#include <iosfwd>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
    static constexpr const char* BusStartFilter = "/mdvwb/buses/+/start";
    static constexpr const char* BusStopFilter = "/mdvwb/buses/+/stop";
    static constexpr const char* BusRestartFilter = "/mdvwb/buses/+/restart";
    static constexpr const char* BusStatusGetFilter = "/mdvwb/buses/+/status/get";
    static constexpr const char* BusDiscoveryStartFilter = "/mdvwb/buses/+/discovery/start";

    ManagerMqttService(
        mdv::IMqttClient& client,
        std::filesystem::path configPath,
        ServiceSyncPaths servicePaths,
        CommandRunner& commandRunner,
        DiscoveryRunner* discoveryRunner = nullptr);

    void Start();
    [[nodiscard]] std::optional<ManagerMqttResult> ProcessOne();
    [[nodiscard]] std::size_t PendingCount() const;

private:
    enum class IncomingType {
        Configuration,
        BusStart,
        BusStop,
        BusRestart,
        BusStatus,
        BusDiscovery,
    };

    struct IncomingCommand {
        IncomingType type = IncomingType::Configuration;
        std::optional<int> busId;
        mdv::MqttMessage message;
    };

    void Enqueue(mdv::MqttMessage message);
    [[nodiscard]] static std::optional<IncomingCommand> ParseIncoming(
        mdv::MqttMessage message);
    [[nodiscard]] ManagerMqttResult ProcessConfiguration(
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessBusCommand(
        IncomingType type,
        int busId,
        const mdv::MqttMessage& message);
    [[nodiscard]] ManagerMqttResult ProcessDiscovery(
        int busId,
        const mdv::MqttMessage& message);

    void PublishCurrentConfig();
    void PublishReadyStatus(std::size_t busCount, std::size_t enabledCount);
    void PublishErrorStatus(std::string_view message);
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
    ServiceSyncPaths servicePaths_;
    CommandRunner& commandRunner_;
    DiscoveryRunner* discoveryRunner_ = nullptr;
    mutable std::mutex mutex_;
    std::deque<IncomingCommand> inbox_;
    bool started_ = false;
};

int RunManagerMqttDaemon(
    const std::filesystem::path& configPath,
    std::ostream& output,
    std::ostream& errors);

}  // namespace mdvwb
