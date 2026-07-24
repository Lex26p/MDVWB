#pragma once

#include "mdv_mqtt.h"
#include "mdv_schedules_config.h"

#include <chrono>
#include <cstddef>
#include <deque>
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
    [[nodiscard]] virtual std::chrono::steady_clock::time_point MonotonicNow() const = 0;
};

class SystemSchedulerClock final : public SchedulerClock {
public:
    [[nodiscard]] SchedulerLocalMinute LocalMinute() const override;
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
    };

    struct QueuedRun {
        ScheduleEntry schedule;
        std::string source;
        std::string minuteKey;
    };

    struct ExpectedFact {
        std::string key;
        std::string expected;
        bool confirmed = false;
    };

    struct ActiveRun {
        QueuedRun run;
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

    void ReloadFromDisk(bool force);
    void ValidateSelected(const ScheduleEntry& schedule) const;
    void QueueAutomaticSchedules(const SchedulerLocalMinute& minute);
    void QueueRun(const ScheduleEntry& schedule, std::string source, std::string minuteKey);
    void StartNextRun();
    void PublishCommands(ActiveRun& run);
    void UpdateConfirmation(std::string_view key, std::string_view payload);
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
    std::deque<IncomingMessage> inbox_;
    std::deque<QueuedRun> runQueue_;
    std::optional<ActiveRun> active_;
    std::map<std::string, std::string> latestFacts_;
    std::map<std::string, std::string> lastAutomaticMinute_;
    SchedulesConfig schedules_;
    std::optional<std::filesystem::file_time_type> schedulesWriteTime_;
    bool started_ = false;
};

int RunSchedulerDaemon(
    const SchedulerPaths& paths,
    std::ostream& output,
    std::ostream& errors);

} // namespace mdvwb
