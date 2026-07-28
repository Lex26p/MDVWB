#pragma once

#include "mdv_mqtt.h"
#include "mdv_schedules_config.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mdvwb {

struct SchedulerLocalMinute {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int weekday = 1; // Monday=1 .. Sunday=7
    [[nodiscard]] std::string DateText() const;
    [[nodiscard]] std::string TimeText() const;
    [[nodiscard]] std::string Key() const;
};

class SchedulerClock {
public:
    virtual ~SchedulerClock() = default;
    [[nodiscard]] virtual SchedulerLocalMinute LocalMinute() const = 0;
    [[nodiscard]] virtual std::int64_t UnixTimeSeconds() const = 0;
    [[nodiscard]] virtual std::chrono::steady_clock::time_point MonotonicNow() const = 0;
};

class SystemSchedulerClock final : public SchedulerClock {
public:
    [[nodiscard]] SchedulerLocalMinute LocalMinute() const override;
    [[nodiscard]] std::int64_t UnixTimeSeconds() const override;
    [[nodiscard]] std::chrono::steady_clock::time_point MonotonicNow() const override;
};

struct SchedulerPaths {
    std::filesystem::path schedules = "/etc/mdvwb/schedules.json";
    std::filesystem::path buses = "/etc/mdvwb/buses.json";
    std::filesystem::path dashboard = "/etc/mdvwb/dashboard.json";
    std::filesystem::path state = "/var/lib/mdvwb/scheduler-state.tsv";
    int confirmationTimeoutSeconds = 10;
};

struct SchedulerProcessResult {
    bool success = false;
    std::string command;
    std::string scheduleId;
    std::string message;
};

class SchedulerService final {
public:
    static constexpr const char* ConfigTopic = "/mdvwb/schedules/config";
    static constexpr const char* ExecuteFilter = "/mdvwb/schedules/+/execute";
    static constexpr const char* FactsFilter = "/devices/+/controls/+";
    static constexpr const char* StatusTopic = "/mdvwb/scheduler/status";

    static constexpr std::size_t MaximumPendingMessages = 1024U;
    static constexpr std::size_t MaximumPendingBytes = 2U * 1024U * 1024U;
    static constexpr std::size_t MaximumQueuedRuns = 128U;
    static constexpr std::size_t MaximumQueuedRunBytes = 256U * 1024U;

    SchedulerService(
        mdv::IMqttClient& client,
        SchedulerPaths paths,
        SchedulerClock& clock);
    void Start();
    [[nodiscard]] std::optional<SchedulerProcessResult> ProcessOne();
    void Tick();
    [[nodiscard]] std::size_t PendingCount() const;
    [[nodiscard]] bool HasActiveRun() const noexcept;

private:
    enum class IncomingType {
        Configuration,
        Execute,
        Fact,
    };

    struct IncomingMessage {
        IncomingType type = IncomingType::Configuration;
        std::string scheduleId;
        std::string factKey;
        mdv::MqttMessage message;

        friend bool SameQueueKey(
            const IncomingMessage& left,
            const IncomingMessage& right) noexcept
        {
            return left.message.topic == right.message.topic;
        }

        friend std::size_t QueueItemBytes(
            const IncomingMessage& incoming) noexcept
        {
            return sizeof(IncomingMessage) + incoming.scheduleId.size() +
                incoming.factKey.size() + incoming.message.topic.size() +
                incoming.message.payload.size();
        }
    };

    struct QueuedRun {
        std::string scheduleId;
        int configRevision = 0;
        std::string source;
        std::string minuteKey;

        friend bool SameQueueKey(
            const QueuedRun& left,
            const QueuedRun& right) noexcept
        {
            return left.scheduleId == right.scheduleId;
        }

        friend std::size_t QueueItemBytes(const QueuedRun& run) noexcept
        {
            return sizeof(QueuedRun) + run.scheduleId.size() +
                run.source.size() + run.minuteKey.size();
        }
    };

    struct ExpectedFact {
        std::string key;
        std::string expected;
        std::uint64_t afterSequence = 0;
        bool confirmed = false;
    };

    struct ActiveRun {
        QueuedRun run;
        ScheduleEntry schedule;
        std::size_t commandCount = 0;
        std::vector<ExpectedFact> expected;
        std::chrono::steady_clock::time_point deadline{};
    };

    void EnqueueMessage(mdv::MqttMessage message);
    [[nodiscard]] static std::optional<IncomingMessage> ParseIncoming(
        mdv::MqttMessage message);
    [[nodiscard]] SchedulerProcessResult ProcessConfiguration(
        const mdv::MqttMessage& message);
    [[nodiscard]] SchedulerProcessResult ProcessExecute(
        std::string_view scheduleId,
        const mdv::MqttMessage& message);
    [[nodiscard]] SchedulerProcessResult ProcessFact(
        std::string_view key,
        const mdv::MqttMessage& message);
    [[nodiscard]] bool ReloadFromDisk(
        bool force,
        std::string* errorMessage = nullptr);
    void ValidateSelected(const ScheduleEntry& schedule) const;
    void QueueAutomaticSchedules(const SchedulerLocalMinute& minute);
    void QueueRun(
        std::string_view scheduleId,
        int configRevision,
        std::string source,
        std::string minuteKey);
    void StartNextRun();
    void PublishCommands(ActiveRun& run);
    void UpdateConfirmation(
        std::string_view key,
        std::string_view payload,
        std::uint64_t sequence,
        bool retained);
    [[nodiscard]] bool IsOfflineStatusForActive(
        std::string_view key,
        std::string_view payload) const;
    void CompleteActiveIfReady();
    void FailActive(std::string_view state, std::string_view message);
    void LoadState();
    void SaveState() const;
    void PruneState();
    void PublishStatus(std::string_view state, std::string_view message = {});
    void PublishRunResult(
        const ActiveRun& run,
        bool success,
        std::string_view state,
        std::string_view message) const;
    [[nodiscard]] static bool IsDue(
        const ScheduleEntry& schedule,
        const SchedulerLocalMinute& minute);
    [[nodiscard]] static bool IsMissedOnce(
        const ScheduleEntry& schedule,
        const SchedulerLocalMinute& minute);
    [[nodiscard]] static std::string CommandTopic(
        const ScheduleTarget& target,
        std::string_view control);
    [[nodiscard]] static std::string FactKey(
        const ScheduleTarget& target,
        std::string_view control);

    mdv::IMqttClient& client_;
    SchedulerPaths paths_;
    SchedulerClock& clock_;
    mutable std::mutex mutex_;
    mdv::BoundedLatestQueue<
        IncomingMessage, MaximumPendingMessages, MaximumPendingBytes> inbox_;
    mdv::BoundedLatestQueue<
        QueuedRun, MaximumQueuedRuns, MaximumQueuedRunBytes> runQueue_;
    std::optional<ActiveRun> active_;
    std::uint64_t factSequence_ = 0;
    std::map<std::string, std::string> lastAutomaticMinute_;
    std::map<std::string, std::string> lastAutomaticAttemptMinute_;
    SchedulesConfig schedules_;
    std::optional<std::filesystem::file_time_type> schedulesWriteTime_;
    std::optional<std::filesystem::file_time_type> rejectedSchedulesWriteTime_;
    std::string statusState_ = "starting";
    std::string statusMessage_;
    std::string publishedControllerMinute_;
    bool started_ = false;
};

int RunSchedulerDaemon(
    const SchedulerPaths& paths,
    std::ostream& output,
    std::ostream& errors);

} // namespace mdvwb
