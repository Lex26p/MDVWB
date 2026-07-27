#include "mdvwb_scheduler.h"

#include "mdv_buses_config.h"
#include "mdv_dashboard_config.h"
#include "mdv_mosquitto.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <utility>

namespace mdvwb {
namespace {

constexpr std::string_view SchedulePrefix = "/mdvwb/schedules/";
constexpr std::string_view DevicePrefix = "/devices/Fan-";
std::atomic_bool StopRequested = false;

std::string JsonEscape(std::string_view value)
{
    std::string result;
    result.reserve(value.size() + 8U);
    for (const unsigned char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (character < 0x20U) {
                result.push_back('?');
            }
            else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return result;
}

bool IsSafeId(std::string_view value)
{
    return !value.empty() && value.size() <= 64U &&
        std::all_of(value.begin(), value.end(), [](char character) {
            const unsigned char byte = static_cast<unsigned char>(character);
            return std::isalnum(byte) != 0 || character == '-' || character == '_';
        });
}

std::string TwoDigits(int value)
{
    std::ostringstream output;
    output << std::setw(2) << std::setfill('0') << value;
    return output.str();
}

std::string FourDigits(int value)
{
    std::ostringstream output;
    output << std::setw(4) << std::setfill('0') << value;
    return output.str();
}

std::string ReadStringEnvironment(const char* name, std::string fallback = {})
{
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::string(value);
}

int ReadIntegerEnvironment(const char* name, int fallback)
{
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }

    int parsed = 0;
    const std::string_view text(value);
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size()) {
        throw std::runtime_error(std::string("invalid integer in ") + name);
    }
    return parsed;
}

mdv::MqttConnectionOptions SchedulerMqttOptionsFromEnvironment()
{
    mdv::MqttConnectionOptions options;
    options.host = ReadStringEnvironment("MDVWB_MQTT_HOST", "127.0.0.1");
    options.port = ReadIntegerEnvironment("MDVWB_MQTT_PORT", 1883);
    options.keepAliveSeconds = ReadIntegerEnvironment("MDVWB_MQTT_KEEPALIVE", 60);
    options.clientId = "mdvwb-scheduler";
    options.username = ReadStringEnvironment("MDVWB_MQTT_USER");
    options.password = ReadStringEnvironment("MDVWB_MQTT_PASSWORD");
    options.reconnectDelaySeconds = static_cast<unsigned int>(
        ReadIntegerEnvironment("MDVWB_MQTT_RECONNECT", 1));
    options.reconnectDelayMaxSeconds = static_cast<unsigned int>(
        ReadIntegerEnvironment("MDVWB_MQTT_RECONNECT_MAX", 10));
    return options;
}

void HandleStopSignal(int)
{
    StopRequested.store(true);
}

std::size_t EnabledCount(const SchedulesConfig& config)
{
    return static_cast<std::size_t>(std::count_if(
        config.schedules.begin(),
        config.schedules.end(),
        [](const ScheduleEntry& schedule) { return schedule.enabled; }));
}

std::string ResultTopic(std::string_view scheduleId)
{
    return "/mdvwb/schedules/" + std::string(scheduleId) + "/result";
}

std::string TargetName(const ScheduleTarget& target)
{
    return "Fan-" + std::to_string(target.bus) + "_" +
        std::to_string(target.address);
}

void WriteStateAtomically(
    const std::filesystem::path& path,
    std::string_view content)
{
    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error(
                "cannot write scheduler state temporary file");
        }
        output.write(
            content.data(),
            static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error(
                "cannot flush scheduler state temporary file");
        }
    }

    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot replace scheduler state file");
    }
}

std::string ActionValue(const std::optional<bool>& value)
{
    return *value ? "1" : "0";
}

std::string ActionValue(const std::optional<int>& value)
{
    return std::to_string(*value);
}

bool IsStoredAutomaticMarker(std::string_view value)
{
    return value.size() == 16U ||
        (value.size() == 23U && value.rfind("missed:", 0U) == 0U);
}

} // namespace

std::string SchedulerLocalMinute::DateText() const
{
    return FourDigits(year) + "-" + TwoDigits(month) + "-" + TwoDigits(day);
}

std::string SchedulerLocalMinute::TimeText() const
{
    return TwoDigits(hour) + ":" + TwoDigits(minute);
}

std::string SchedulerLocalMinute::Key() const
{
    return DateText() + "T" + TimeText();
}

SchedulerLocalMinute SystemSchedulerClock::LocalMinute() const
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif

    SchedulerLocalMinute result;
    result.year = local.tm_year + 1900;
    result.month = local.tm_mon + 1;
    result.day = local.tm_mday;
    result.hour = local.tm_hour;
    result.minute = local.tm_min;
    result.weekday = local.tm_wday == 0 ? 7 : local.tm_wday;
    return result;
}

std::chrono::steady_clock::time_point
SystemSchedulerClock::MonotonicNow() const
{
    return std::chrono::steady_clock::now();
}

SchedulerService::SchedulerService(
    mdv::IMqttClient& client,
    SchedulerPaths paths,
    SchedulerClock& clock)
    : client_(client), paths_(std::move(paths)), clock_(clock)
{
    if (paths_.confirmationTimeoutSeconds < 1 ||
        paths_.confirmationTimeoutSeconds > 300) {
        throw std::invalid_argument(
            "confirmation timeout must be in range 1..300 seconds");
    }
}

void SchedulerService::Start()
{
    if (started_) {
        return;
    }

    client_.SetMessageHandler([this](mdv::MqttMessage message) {
        EnqueueMessage(std::move(message));
    });
    client_.Subscribe(ConfigTopic);
    client_.Subscribe(ExecuteFilter);
    client_.Subscribe(FactsFilter);
    LoadState();
    std::string reloadError;
    if (!ReloadFromDisk(true, &reloadError)) {
        throw SchedulesConfigError(reloadError);
    }
    started_ = true;
    PublishStatus("ready", "Scheduler started");
}

void SchedulerService::EnqueueMessage(mdv::MqttMessage message)
{
    const auto parsed = ParseIncoming(std::move(message));
    if (!parsed) {
        return;
    }

    std::lock_guard lock(mutex_);
    inbox_.push_back(*parsed);
}

std::optional<SchedulerService::IncomingMessage>
SchedulerService::ParseIncoming(mdv::MqttMessage message)
{
    if (message.topic == ConfigTopic) {
        return IncomingMessage{
            IncomingType::Configuration, {}, {}, std::move(message)};
    }

    if (message.topic.rfind(SchedulePrefix, 0U) == 0U) {
        const std::string_view suffix(
            message.topic.data() + SchedulePrefix.size(),
            message.topic.size() - SchedulePrefix.size());
        constexpr std::string_view ending = "/execute";
        if (suffix.size() > ending.size() &&
            suffix.substr(suffix.size() - ending.size()) == ending) {
            const std::string_view id =
                suffix.substr(0U, suffix.size() - ending.size());
            if (IsSafeId(id)) {
                return IncomingMessage{
                    IncomingType::Execute,
                    std::string(id),
                    {},
                    std::move(message)};
            }
        }
    }

    if (message.topic.rfind(DevicePrefix, 0U) == 0U) {
        const std::size_t controls =
            message.topic.find("/controls/", DevicePrefix.size());
        if (controls != std::string::npos &&
            message.topic.find('/', controls + 10U) == std::string::npos) {
            return IncomingMessage{
                IncomingType::Fact,
                {},
                message.topic,
                std::move(message)};
        }
    }

    return std::nullopt;
}

std::optional<SchedulerProcessResult> SchedulerService::ProcessOne()
{
    IncomingMessage incoming;
    {
        std::lock_guard lock(mutex_);
        if (inbox_.empty()) {
            return std::nullopt;
        }
        incoming = std::move(inbox_.front());
        inbox_.pop_front();
    }

    switch (incoming.type) {
    case IncomingType::Configuration:
        return ProcessConfiguration(incoming.message);
    case IncomingType::Execute:
        return ProcessExecute(incoming.scheduleId, incoming.message);
    case IncomingType::Fact:
        return ProcessFact(incoming.factKey, incoming.message);
    }
    return std::nullopt;
}

SchedulerProcessResult SchedulerService::ProcessConfiguration(
    const mdv::MqttMessage& message)
{
    (void)message;
    std::string reloadError;
    if (!ReloadFromDisk(true, &reloadError)) {
        const std::string detail =
            "Cannot reload schedules configuration from disk: " +
            reloadError;
        PublishStatus("error", detail);
        return {false, "configuration", {}, detail};
    }

    PublishStatus("ready", "Schedules configuration reloaded from disk");
    return {
        true,
        "configuration",
        {},
        "Schedules configuration reloaded from disk"};
}

SchedulerProcessResult SchedulerService::ProcessExecute(
    std::string_view scheduleId,
    const mdv::MqttMessage& message)
{
    if (message.retained) {
        ActiveRun synthetic;
        synthetic.run.scheduleId = std::string(scheduleId);
        synthetic.run.source = "manual";
        synthetic.schedule.id = std::string(scheduleId);
        PublishRunResult(
            synthetic,
            false,
            "rejected",
            "Retained execute events are ignored");
        return {
            false,
            "execute",
            std::string(scheduleId),
            "Retained execute events are ignored"};
    }

    const ScheduleEntry* schedule = FindSchedule(schedules_, scheduleId);
    if (schedule == nullptr) {
        const std::string detail =
            "Schedule '" + std::string(scheduleId) + "' does not exist";
        ActiveRun synthetic;
        synthetic.run.scheduleId = std::string(scheduleId);
        synthetic.run.source = "manual";
        synthetic.schedule.id = std::string(scheduleId);
        PublishRunResult(synthetic, false, "rejected", detail);
        return {false, "execute", std::string(scheduleId), detail};
    }

    try {
        ValidateSelected(*schedule);
        QueueRun(schedule->id, schedules_.revision, "manual", {});

        ActiveRun accepted;
        accepted.run.scheduleId = schedule->id;
        accepted.run.configRevision = schedules_.revision;
        accepted.run.source = "manual";
        accepted.schedule = *schedule;
        PublishRunResult(
            accepted,
            true,
            "queued",
            "Schedule accepted by scheduler");

        StartNextRun();
        return {
            true,
            "execute",
            std::string(scheduleId),
            "Schedule accepted for execution"};
    }
    catch (const std::exception& error) {
        const std::string detail =
            std::string("Cannot execute schedule: ") + error.what();
        ActiveRun synthetic;
        synthetic.run.scheduleId = schedule->id;
        synthetic.run.configRevision = schedules_.revision;
        synthetic.run.source = "manual";
        synthetic.schedule = *schedule;
        PublishRunResult(synthetic, false, "rejected", detail);
        return {false, "execute", std::string(scheduleId), detail};
    }
}

SchedulerProcessResult SchedulerService::ProcessFact(
    std::string_view key,
    const mdv::MqttMessage& message)
{
    const std::uint64_t sequence = ++factSequence_;
    latestFacts_[std::string(key)] = LatestFact{
        message.payload,
        sequence,
        message.retained};

    if (IsOfflineStatusForActive(key, message.payload)) {
        const std::string device = std::string(
            key.substr(0U, key.find("/controls/")));
        FailActive(
            "failed",
            "Target device reported offline: " + device);
        return {false, "fact", {}, "Active target reported offline"};
    }

    UpdateConfirmation(
        key,
        message.payload,
        sequence,
        message.retained);
    CompleteActiveIfReady();
    return {true, "fact", {}, "Fact updated"};
}

void SchedulerService::Tick()
{
    const SchedulerLocalMinute minute = clock_.LocalMinute();
    std::string reloadError;
    if (!ReloadFromDisk(false, &reloadError) && !reloadError.empty()) {
        PublishStatus(
            "error",
            "Cannot reload schedules configuration from disk: " +
                reloadError);
    }
    QueueAutomaticSchedules(minute);
    StartNextRun();
    CompleteActiveIfReady();

    if (active_ && clock_.MonotonicNow() >= active_->deadline) {
        const std::size_t confirmed = static_cast<std::size_t>(
            std::count_if(
                active_->expected.begin(),
                active_->expected.end(),
                [](const ExpectedFact& fact) {
                    return fact.confirmed;
                }));
        FailActive(
            "timeout",
            "Confirmation timeout: " + std::to_string(confirmed) + "/" +
                std::to_string(active_->expected.size()) +
                " values confirmed");
    }

    StartNextRun();

    if (publishedControllerMinute_ != minute.Key()) {
        const std::string currentState = statusState_;
        const std::string currentMessage = statusMessage_;
        PublishStatus(currentState, currentMessage);
    }
}

std::size_t SchedulerService::PendingCount() const
{
    std::lock_guard lock(mutex_);
    return inbox_.size() + runQueue_.size() + (active_ ? 1U : 0U);
}

bool SchedulerService::HasActiveRun() const noexcept
{
    return active_.has_value();
}

bool SchedulerService::ReloadFromDisk(
    bool force,
    std::string* errorMessage)
{
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }

    std::error_code error;
    const auto writeTime =
        std::filesystem::last_write_time(paths_.schedules, error);
    if (error) {
        if (force && errorMessage != nullptr) {
            *errorMessage =
                "cannot stat schedules configuration '" +
                paths_.schedules.string() + "'";
        }
        return !force;
    }

    if (!force && schedulesWriteTime_ &&
        *schedulesWriteTime_ == writeTime) {
        return true;
    }
    if (!force && rejectedSchedulesWriteTime_ &&
        *rejectedSchedulesWriteTime_ == writeTime) {
        return true;
    }

    try {
        SchedulesConfig loaded = LoadSchedulesConfig(paths_.schedules);
        ValidateScheduleReferences(
            loaded,
            LoadBusesConfig(paths_.buses),
            LoadDashboardCollection(paths_.dashboard));
        schedules_ = std::move(loaded);
        schedulesWriteTime_ = writeTime;
        rejectedSchedulesWriteTime_.reset();
        lastAutomaticAttemptMinute_.clear();
        PruneState();
        return true;
    }
    catch (const std::exception& exception) {
        rejectedSchedulesWriteTime_ = writeTime;
        if (errorMessage != nullptr) {
            *errorMessage = exception.what();
        }
        return false;
    }
}

void SchedulerService::ValidateSelected(
    const ScheduleEntry& schedule) const
{
    SchedulesConfig selected;
    selected.revision = schedules_.revision;
    selected.schedules.push_back(schedule);
    ValidateScheduleReferences(
        selected,
        LoadBusesConfig(paths_.buses),
        LoadDashboardCollection(paths_.dashboard));
}

void SchedulerService::QueueAutomaticSchedules(
    const SchedulerLocalMinute& minute)
{
    const std::string minuteKey = minute.Key();
    for (const ScheduleEntry& schedule : schedules_.schedules) {
        if (!schedule.enabled) {
            continue;
        }

        if (IsMissedOnce(schedule, minute)) {
            const std::string scheduledKey =
                schedule.date + "T" + schedule.time;
            const std::string missedKey = "missed:" + scheduledKey;
            const auto previous = lastAutomaticMinute_.find(schedule.id);
            if (previous != lastAutomaticMinute_.end() &&
                (previous->second == scheduledKey ||
                 previous->second == missedKey)) {
                continue;
            }

            lastAutomaticMinute_[schedule.id] = missedKey;
            SaveState();
            ActiveRun synthetic;
            synthetic.run.scheduleId = schedule.id;
            synthetic.run.configRevision = schedules_.revision;
            synthetic.run.source = "automatic";
            synthetic.run.minuteKey = minuteKey;
            synthetic.schedule = schedule;
            PublishRunResult(
                synthetic,
                false,
                "missed",
                "One-time schedule was missed while the scheduler was not running");
            continue;
        }

        if (!IsDue(schedule, minute)) {
            continue;
        }

        const auto previous = lastAutomaticMinute_.find(schedule.id);
        if (previous != lastAutomaticMinute_.end() &&
            previous->second == minuteKey) {
            continue;
        }
        const auto attempted = lastAutomaticAttemptMinute_.find(schedule.id);
        if (attempted != lastAutomaticAttemptMinute_.end() &&
            attempted->second == minuteKey) {
            continue;
        }

        try {
            ValidateSelected(schedule);
            lastAutomaticMinute_[schedule.id] = minuteKey;
            SaveState();
            QueueRun(
                schedule.id,
                schedules_.revision,
                "automatic",
                minuteKey);
        }
        catch (const std::exception& error) {
            lastAutomaticAttemptMinute_[schedule.id] = minuteKey;
            ActiveRun synthetic;
            synthetic.run.scheduleId = schedule.id;
            synthetic.run.configRevision = schedules_.revision;
            synthetic.run.source = "automatic";
            synthetic.run.minuteKey = minuteKey;
            synthetic.schedule = schedule;
            PublishRunResult(
                synthetic,
                false,
                "rejected",
                error.what());
        }
    }
}

void SchedulerService::QueueRun(
    std::string_view scheduleId,
    int configRevision,
    std::string source,
    std::string minuteKey)
{
    runQueue_.push_back(QueuedRun{
        std::string(scheduleId),
        configRevision,
        std::move(source),
        std::move(minuteKey)});
    PublishStatus("ready", "Schedule queued");
}

void SchedulerService::StartNextRun()
{
    while (!active_ && !runQueue_.empty()) {
        QueuedRun request = std::move(runQueue_.front());
        runQueue_.pop_front();

        ActiveRun candidate;
        candidate.run = request;
        candidate.schedule.id = request.scheduleId;

        const auto reject = [this, &candidate](std::string message) {
            PublishRunResult(
                candidate,
                false,
                "rejected",
                message);
            PublishStatus("warning", message);
        };

        if (request.configRevision != schedules_.revision) {
            reject(
                "Queued schedule configuration changed before execution");
            continue;
        }

        const ScheduleEntry* current =
            FindSchedule(schedules_, request.scheduleId);
        if (current == nullptr) {
            reject("Queued schedule no longer exists");
            continue;
        }
        if (request.source == "automatic" && !current->enabled) {
            candidate.schedule = *current;
            reject("Queued automatic schedule was disabled before execution");
            continue;
        }

        candidate.schedule = *current;
        try {
            ValidateSelected(candidate.schedule);
        }
        catch (const std::exception& error) {
            reject(
                std::string("Queued schedule is no longer valid: ") +
                error.what());
            continue;
        }

        candidate.deadline = clock_.MonotonicNow() +
            std::chrono::seconds(paths_.confirmationTimeoutSeconds);
        active_ = std::move(candidate);

        try {
            PublishCommands(*active_);
            PublishRunResult(
                *active_,
                true,
                "executing",
                active_->commandCount == 0U
                    ? "Requested factual state is already satisfied"
                    : "Commands published; waiting for factual state");
            PublishStatus(
                "executing",
                active_->commandCount == 0U
                    ? "Requested factual state is already satisfied"
                    : "Waiting for factual state confirmation");
            CompleteActiveIfReady();
        }
        catch (const std::exception& error) {
            FailActive("failed", error.what());
        }
    }
}

void SchedulerService::PublishCommands(ActiveRun& run)
{
    for (const ScheduleTarget& target : run.schedule.targets) {
        const auto status = latestFacts_.find(FactKey(target, "Status"));
        if (status != latestFacts_.end() && status->second.payload == "7") {
            throw std::runtime_error(
                "target device is offline: " + TargetName(target));
        }
    }

    const auto publish = [this, &run](
                             const ScheduleTarget& target,
                             std::string_view control,
                             std::string payload) {
        const std::string key = FactKey(target, control);
        const auto current = latestFacts_.find(key);
        const auto status = latestFacts_.find(FactKey(target, "Status"));
        const bool knownOnline =
            status != latestFacts_.end() && status->second.payload != "7";
        const bool alreadySatisfied =
            knownOnline && current != latestFacts_.end() &&
            current->second.payload == payload;

        ExpectedFact expected;
        expected.key = key;
        expected.expected = payload;
        expected.afterSequence = factSequence_;
        expected.confirmed = alreadySatisfied;
        run.expected.push_back(std::move(expected));

        if (alreadySatisfied) {
            return;
        }

        client_.Publish(
            CommandTopic(target, control),
            payload,
            false);
        ++run.commandCount;
    };

    for (const ScheduleTarget& target : run.schedule.targets) {
        // Power is sent last so the final transaction leaves the requested on/off state.
        if (run.schedule.actions.mode) {
            publish(
                target,
                "Mode",
                ActionValue(run.schedule.actions.mode));
        }
        if (run.schedule.actions.speed) {
            publish(
                target,
                "Speed",
                ActionValue(run.schedule.actions.speed));
        }
        if (run.schedule.actions.setTemp) {
            publish(
                target,
                "SetTemp",
                ActionValue(run.schedule.actions.setTemp));
        }
        if (run.schedule.actions.power) {
            publish(
                target,
                "Power",
                ActionValue(run.schedule.actions.power));
        }
    }

    if (run.expected.empty()) {
        throw std::runtime_error(
            "schedule produced no expected factual values");
    }
}

void SchedulerService::UpdateConfirmation(
    std::string_view key,
    std::string_view payload,
    std::uint64_t sequence,
    bool retained)
{
    if (!active_) {
        return;
    }

    for (ExpectedFact& expected : active_->expected) {
        if (!expected.confirmed &&
            expected.key == key &&
            expected.expected == payload &&
            sequence > expected.afterSequence &&
            !retained) {
            expected.confirmed = true;
        }
    }
}

bool SchedulerService::IsOfflineStatusForActive(
    std::string_view key,
    std::string_view payload) const
{
    if (!active_ || payload != "7") {
        return false;
    }

    for (const ScheduleTarget& target : active_->schedule.targets) {
        if (FactKey(target, "Status") == key) {
            return true;
        }
    }
    return false;
}

void SchedulerService::CompleteActiveIfReady()
{
    if (!active_ || active_->expected.empty()) {
        return;
    }

    const bool complete = std::all_of(
        active_->expected.begin(),
        active_->expected.end(),
        [](const ExpectedFact& fact) {
            return fact.confirmed;
        });
    if (!complete) {
        return;
    }

    PublishRunResult(
        *active_,
        true,
        "completed",
        "All factual values confirmed");
    active_.reset();
    PublishStatus("ready", "Schedule completed");
}

void SchedulerService::FailActive(
    std::string_view state,
    std::string_view message)
{
    if (!active_) {
        return;
    }

    PublishRunResult(*active_, false, state, message);
    active_.reset();
    PublishStatus("warning", message);
}

void SchedulerService::LoadState()
{
    lastAutomaticMinute_.clear();
    std::ifstream input(paths_.state, std::ios::binary);
    if (!input) {
        return;
    }

    std::string line;
    while (std::getline(input, line)) {
        const std::size_t separator = line.find('\t');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string id = line.substr(0U, separator);
        const std::string minuteKey = line.substr(separator + 1U);
        if (IsSafeId(id) && IsStoredAutomaticMarker(minuteKey)) {
            lastAutomaticMinute_[id] = minuteKey;
        }
    }
}

void SchedulerService::SaveState() const
{
    std::ostringstream output;
    for (const auto& [id, minuteKey] : lastAutomaticMinute_) {
        output << id << '\t' << minuteKey << '\n';
    }
    WriteStateAtomically(paths_.state, output.str());
}

void SchedulerService::PruneState()
{
    std::set<std::string> validIds;
    for (const ScheduleEntry& schedule : schedules_.schedules) {
        validIds.insert(schedule.id);
    }

    bool changed = false;
    for (auto iterator = lastAutomaticMinute_.begin();
         iterator != lastAutomaticMinute_.end();) {
        if (!validIds.contains(iterator->first)) {
            iterator = lastAutomaticMinute_.erase(iterator);
            changed = true;
        }
        else {
            ++iterator;
        }
    }
    for (auto iterator = lastAutomaticAttemptMinute_.begin();
         iterator != lastAutomaticAttemptMinute_.end();) {
        if (!validIds.contains(iterator->first)) {
            iterator = lastAutomaticAttemptMinute_.erase(iterator);
        }
        else {
            ++iterator;
        }
    }

    if (changed) {
        SaveState();
    }
}

void SchedulerService::PublishStatus(
    std::string_view state,
    std::string_view message)
{
    const SchedulerLocalMinute controllerMinute = clock_.LocalMinute();
    statusState_ = std::string(state);
    statusMessage_ = std::string(message);
    publishedControllerMinute_ = controllerMinute.Key();

    std::string payload =
        "{\"state\":\"" + JsonEscape(state) + "\"" +
        ",\"revision\":" + std::to_string(schedules_.revision) +
        ",\"schedules\":" + std::to_string(schedules_.schedules.size()) +
        ",\"enabled\":" + std::to_string(EnabledCount(schedules_)) +
        ",\"queued\":" + std::to_string(runQueue_.size()) +
        ",\"active\":" + std::string(active_ ? "true" : "false") +
        ",\"controllerDate\":\"" + controllerMinute.DateText() + "\"" +
        ",\"controllerTime\":\"" + controllerMinute.TimeText() + "\"" +
        ",\"controllerMinute\":\"" + controllerMinute.Key() + "\"" +
        ",\"controllerWeekday\":" +
            std::to_string(controllerMinute.weekday);
    if (active_) {
        payload +=
            ",\"activeSchedule\":\"" +
            JsonEscape(active_->schedule.id) + "\"";
    }
    if (!message.empty()) {
        payload +=
            ",\"message\":\"" + JsonEscape(message) + "\"";
    }
    payload += '}';
    client_.Publish(StatusTopic, payload, true);
}

void SchedulerService::PublishRunResult(
    const ActiveRun& run,
    bool success,
    std::string_view state,
    std::string_view message) const
{
    const std::size_t confirmed = static_cast<std::size_t>(
        std::count_if(
            run.expected.begin(),
            run.expected.end(),
            [](const ExpectedFact& fact) {
                return fact.confirmed;
            }));
    const SchedulerLocalMinute controllerMinute = clock_.LocalMinute();
    const std::string payload =
        std::string("{\"success\":") + (success ? "true" : "false") +
        ",\"scheduleId\":\"" + JsonEscape(run.schedule.id) + "\"" +
        ",\"state\":\"" + JsonEscape(state) + "\"" +
        ",\"source\":\"" + JsonEscape(run.run.source) + "\"" +
        ",\"origin\":\"scheduler\"" +
        ",\"controllerMinute\":\"" + controllerMinute.Key() + "\"" +
        ",\"commands\":" + std::to_string(run.commandCount) +
        ",\"confirmed\":" + std::to_string(confirmed) +
        ",\"message\":\"" + JsonEscape(message) + "\"}";
    client_.Publish(
        ResultTopic(run.schedule.id),
        payload,
        false);
}

bool SchedulerService::IsDue(
    const ScheduleEntry& schedule,
    const SchedulerLocalMinute& minute)
{
    if (schedule.time != minute.TimeText()) {
        return false;
    }
    if (schedule.kind == ScheduleKind::Once) {
        return schedule.date == minute.DateText();
    }
    return std::find(
               schedule.days.begin(),
               schedule.days.end(),
               minute.weekday) != schedule.days.end();
}

bool SchedulerService::IsMissedOnce(
    const ScheduleEntry& schedule,
    const SchedulerLocalMinute& minute)
{
    if (schedule.kind != ScheduleKind::Once) {
        return false;
    }
    return schedule.date + "T" + schedule.time < minute.Key();
}

std::string SchedulerService::CommandTopic(
    const ScheduleTarget& target,
    std::string_view control)
{
    return "/devices/Fan-" + std::to_string(target.bus) + "_" +
        std::to_string(target.address) + "/controls/" +
        std::string(control) + "/on1";
}

std::string SchedulerService::FactKey(
    const ScheduleTarget& target,
    std::string_view control)
{
    return "/devices/Fan-" + std::to_string(target.bus) + "_" +
        std::to_string(target.address) + "/controls/" +
        std::string(control);
}

int RunSchedulerDaemon(
    const SchedulerPaths& paths,
    std::ostream& output,
    std::ostream& errors)
{
    if (!mdv::MosquittoMqttClient::IsSupported()) {
        errors <<
            "SCHEDULER_ERROR: libmosquitto support is not available in this build\n";
        return 1;
    }

    try {
        StopRequested.store(false);
        std::signal(SIGINT, HandleStopSignal);
        std::signal(SIGTERM, HandleStopSignal);
        mdv::MosquittoMqttClient client(
            SchedulerMqttOptionsFromEnvironment());
        SystemSchedulerClock clock;
        SchedulerService service(client, paths, clock);
        service.Start();
        client.Start();
        output <<
            "SCHEDULER_STARTED schedules=" <<
            paths.schedules.string() << '\n';

        while (!StopRequested.load()) {
            while (service.ProcessOne().has_value()) {
            }
            service.Tick();
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100));
        }

        client.Stop();
        output << "SCHEDULER_STOPPED\n";
        return 0;
    }
    catch (const std::exception& error) {
        errors << "SCHEDULER_ERROR: " << error.what() << '\n';
        return 1;
    }
}

} // namespace mdvwb
