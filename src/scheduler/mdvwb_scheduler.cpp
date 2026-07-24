#include "mdvwb_scheduler.h"

#include "mdv_buses_config.h"
#include "mdv_dashboard_config.h"
#include "mdv_mosquitto.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <ctime>
#include <csignal>
#include <cstdlib>
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

std::string JsonEscape(std::string_view value) {
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
            } else {
                result.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return result;
}

bool IsSafeId(std::string_view value) {
    return !value.empty() && value.size() <= 64U &&
        std::all_of(value.begin(), value.end(), [](char character) {
            const unsigned char byte = static_cast<unsigned char>(character);
            return std::isalnum(byte) != 0 || character == '-' || character == '_';
        });
}

std::string TwoDigits(int value) {
    std::ostringstream output;
    output << std::setw(2) << std::setfill('0') << value;
    return output.str();
}

std::string FourDigits(int value) {
    std::ostringstream output;
    output << std::setw(4) << std::setfill('0') << value;
    return output.str();
}

std::string ReadStringEnvironment(const char* name, std::string fallback = {}) {
    const char* value = std::getenv(name);
    return value == nullptr ? fallback : std::string(value);
}

int ReadIntegerEnvironment(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    int parsed = 0;
    const std::string_view text(value);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        throw std::runtime_error(std::string("invalid integer in ") + name);
    }
    return parsed;
}

mdv::MqttConnectionOptions SchedulerMqttOptionsFromEnvironment() {
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

void HandleStopSignal(int) {
    StopRequested.store(true);
}

std::size_t EnabledCount(const SchedulesConfig& config) {
    return static_cast<std::size_t>(std::count_if(
        config.schedules.begin(), config.schedules.end(),
        [](const ScheduleEntry& schedule) { return schedule.enabled; }));
}

std::string ResultTopic(std::string_view scheduleId) {
    return "/mdvwb/schedules/" + std::string(scheduleId) + "/result";
}


void WriteStateAtomically(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot write scheduler state temporary file");
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            throw std::runtime_error("cannot flush scheduler state temporary file");
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

std::string ActionValue(const std::optional<bool>& value) {
    return *value ? "1" : "0";
}

std::string ActionValue(const std::optional<int>& value) {
    return std::to_string(*value);
}

} // namespace

std::string SchedulerLocalMinute::DateText() const {
    return FourDigits(year) + "-" + TwoDigits(month) + "-" + TwoDigits(day);
}

std::string SchedulerLocalMinute::TimeText() const {
    return TwoDigits(hour) + ":" + TwoDigits(minute);
}

std::string SchedulerLocalMinute::Key() const {
    return DateText() + "T" + TimeText();
}

SchedulerLocalMinute SystemSchedulerClock::LocalMinute() const {
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

std::chrono::steady_clock::time_point SystemSchedulerClock::MonotonicNow() const {
    return std::chrono::steady_clock::now();
}

SchedulerService::SchedulerService(
    mdv::IMqttClient& client,
    SchedulerPaths paths,
    SchedulerClock& clock)
    : client_(client), paths_(std::move(paths)), clock_(clock) {
    if (paths_.confirmationTimeoutSeconds < 1 ||
        paths_.confirmationTimeoutSeconds > 300) {
        throw std::invalid_argument("confirmation timeout must be in range 1..300 seconds");
    }
}

void SchedulerService::Start() {
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
    ReloadFromDisk(true);
    started_ = true;
    PublishStatus("ready", "Scheduler started");
}

void SchedulerService::EnqueueMessage(mdv::MqttMessage message) {
    const auto parsed = ParseIncoming(std::move(message));
    if (!parsed) {
        return;
    }
    std::lock_guard lock(mutex_);
    inbox_.push_back(*parsed);
}

std::optional<SchedulerService::IncomingMessage> SchedulerService::ParseIncoming(
    mdv::MqttMessage message) {
    if (message.topic == ConfigTopic) {
        return IncomingMessage{IncomingType::Configuration, {}, {}, std::move(message)};
    }
    if (message.topic.rfind(SchedulePrefix, 0U) == 0U) {
        const std::string_view suffix(message.topic.data() + SchedulePrefix.size(),
                                      message.topic.size() - SchedulePrefix.size());
        constexpr std::string_view ending = "/execute";
        if (suffix.size() > ending.size() &&
            suffix.substr(suffix.size() - ending.size()) == ending) {
            const std::string_view id = suffix.substr(0U, suffix.size() - ending.size());
            if (IsSafeId(id)) {
                return IncomingMessage{
                    IncomingType::Execute, std::string(id), {}, std::move(message)};
            }
        }
    }
    if (message.topic.rfind(DevicePrefix, 0U) == 0U) {
        const std::size_t controls = message.topic.find("/controls/", DevicePrefix.size());
        if (controls != std::string::npos &&
            message.topic.find('/', controls + 10U) == std::string::npos) {
            return IncomingMessage{
                IncomingType::Fact, {}, message.topic, std::move(message)};
        }
    }
    return std::nullopt;
}

std::optional<SchedulerProcessResult> SchedulerService::ProcessOne() {
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
    const mdv::MqttMessage& message) {
    try {
        SchedulesConfig submitted = ParseSchedulesConfig(message.payload);
        ValidateScheduleReferences(
            submitted, LoadBusesConfig(paths_.buses), LoadDashboardCollection(paths_.dashboard));
        schedules_ = std::move(submitted);
        PruneState();
        PublishStatus("ready", "Schedules configuration updated");
        return {true, "configuration", {}, "Schedules configuration updated"};
    } catch (const std::exception& error) {
        const std::string detail = std::string("Cannot apply schedules configuration: ") + error.what();
        PublishStatus("error", detail);
        return {false, "configuration", {}, detail};
    }
}

SchedulerProcessResult SchedulerService::ProcessExecute(
    std::string_view scheduleId,
    const mdv::MqttMessage& message) {
    if (message.retained) {
        ActiveRun synthetic;
        synthetic.run.schedule.id = std::string(scheduleId);
        synthetic.run.source = "manual";
        PublishRunResult(synthetic, false, "rejected", "Retained execute events are ignored");
        return {false, "execute", std::string(scheduleId),
                "Retained execute events are ignored"};
    }
    const ScheduleEntry* schedule = FindSchedule(schedules_, scheduleId);
    if (schedule == nullptr) {
        const std::string detail = "Schedule '" + std::string(scheduleId) + "' does not exist";
        ActiveRun synthetic;
        synthetic.run.schedule.id = std::string(scheduleId);
        synthetic.run.source = "manual";
        PublishRunResult(synthetic, false, "rejected", detail);
        return {false, "execute", std::string(scheduleId), detail};
    }
    try {
        ValidateSelected(*schedule);
        QueueRun(*schedule, "manual", {});
        StartNextRun();
        return {true, "execute", std::string(scheduleId), "Schedule accepted for execution"};
    } catch (const std::exception& error) {
        const std::string detail = std::string("Cannot execute schedule: ") + error.what();
        ActiveRun synthetic;
        synthetic.run.schedule = *schedule;
        synthetic.run.source = "manual";
        PublishRunResult(synthetic, false, "rejected", detail);
        return {false, "execute", std::string(scheduleId), detail};
    }
}

SchedulerProcessResult SchedulerService::ProcessFact(
    std::string_view key,
    const mdv::MqttMessage& message) {
    latestFacts_[std::string(key)] = message.payload;
    UpdateConfirmation(key, message.payload);
    CompleteActiveIfReady();
    return {true, "fact", {}, "Fact updated"};
}

void SchedulerService::Tick() {
    ReloadFromDisk(false);
    QueueAutomaticSchedules(clock_.LocalMinute());
    StartNextRun();
    CompleteActiveIfReady();
    if (active_ && clock_.MonotonicNow() >= active_->deadline) {
        const std::size_t confirmed = static_cast<std::size_t>(std::count_if(
            active_->expected.begin(), active_->expected.end(),
            [](const ExpectedFact& fact) { return fact.confirmed; }));
        FailActive(
            "timeout",
            "Confirmation timeout: " + std::to_string(confirmed) + "/" +
                std::to_string(active_->expected.size()) + " values confirmed");
    }
    StartNextRun();
}

std::size_t SchedulerService::PendingCount() const {
    std::lock_guard lock(mutex_);
    return inbox_.size() + runQueue_.size() + (active_ ? 1U : 0U);
}

bool SchedulerService::HasActiveRun() const noexcept {
    return active_.has_value();
}

void SchedulerService::ReloadFromDisk(bool force) {
    std::error_code error;
    const auto writeTime = std::filesystem::last_write_time(paths_.schedules, error);
    if (error) {
        if (force) {
            throw SchedulesConfigError(
                "cannot stat schedules configuration '" + paths_.schedules.string() + "'");
        }
        return;
    }
    if (!force && schedulesWriteTime_ && *schedulesWriteTime_ == writeTime) {
        return;
    }
    SchedulesConfig loaded = LoadSchedulesConfig(paths_.schedules);
    ValidateScheduleReferences(
        loaded, LoadBusesConfig(paths_.buses), LoadDashboardCollection(paths_.dashboard));
    schedules_ = std::move(loaded);
    schedulesWriteTime_ = writeTime;
    PruneState();
}

void SchedulerService::ValidateSelected(const ScheduleEntry& schedule) const {
    SchedulesConfig selected;
    selected.revision = schedules_.revision;
    selected.schedules.push_back(schedule);
    ValidateScheduleReferences(
        selected, LoadBusesConfig(paths_.buses), LoadDashboardCollection(paths_.dashboard));
}

void SchedulerService::QueueAutomaticSchedules(const SchedulerLocalMinute& minute) {
    for (const ScheduleEntry& schedule : schedules_.schedules) {
        if (!schedule.enabled || !IsDue(schedule, minute)) {
            continue;
        }
        const std::string minuteKey = minute.Key();
        const auto previous = lastAutomaticMinute_.find(schedule.id);
        if (previous != lastAutomaticMinute_.end() && previous->second == minuteKey) {
            continue;
        }
        lastAutomaticMinute_[schedule.id] = minuteKey;
        SaveState();
        try {
            ValidateSelected(schedule);
            QueueRun(schedule, "automatic", minuteKey);
        } catch (const std::exception& error) {
            ActiveRun synthetic;
            synthetic.run.schedule = schedule;
            synthetic.run.source = "automatic";
            synthetic.run.minuteKey = minuteKey;
            PublishRunResult(synthetic, false, "rejected", error.what());
        }
    }
}

void SchedulerService::QueueRun(
    const ScheduleEntry& schedule,
    std::string source,
    std::string minuteKey) {
    runQueue_.push_back(QueuedRun{schedule, std::move(source), std::move(minuteKey)});
    PublishStatus("ready", "Schedule queued");
}

void SchedulerService::StartNextRun() {
    if (active_ || runQueue_.empty()) {
        return;
    }
    ActiveRun run;
    run.run = std::move(runQueue_.front());
    runQueue_.pop_front();
    run.deadline = clock_.MonotonicNow() +
        std::chrono::seconds(paths_.confirmationTimeoutSeconds);
    active_ = std::move(run);
    try {
        ValidateSelected(active_->run.schedule);
        PublishCommands(*active_);
        PublishRunResult(*active_, true, "executing", "Commands published; waiting for factual state");
        PublishStatus("executing", "Waiting for factual state confirmation");
        CompleteActiveIfReady();
    } catch (const std::exception& error) {
        FailActive("failed", error.what());
    }
}

void SchedulerService::PublishCommands(ActiveRun& run) {
    const auto publish = [&](const ScheduleTarget& target,
                             std::string_view control,
                             std::string payload) {
        client_.Publish(CommandTopic(target, control), payload, false);
        ExpectedFact expected{FactKey(target, control), std::move(payload), false};
        const auto current = latestFacts_.find(expected.key);
        expected.confirmed = current != latestFacts_.end() && current->second == expected.expected;
        run.expected.push_back(std::move(expected));
        ++run.commandCount;
    };

    for (const ScheduleTarget& target : run.run.schedule.targets) {
        // Power is sent last so the final transaction leaves the requested on/off state.
        if (run.run.schedule.actions.mode) {
            publish(target, "Mode", ActionValue(run.run.schedule.actions.mode));
        }
        if (run.run.schedule.actions.speed) {
            publish(target, "Speed", ActionValue(run.run.schedule.actions.speed));
        }
        if (run.run.schedule.actions.setTemp) {
            publish(target, "SetTemp", ActionValue(run.run.schedule.actions.setTemp));
        }
        if (run.run.schedule.actions.power) {
            publish(target, "Power", ActionValue(run.run.schedule.actions.power));
        }
    }
    if (run.commandCount == 0U) {
        throw std::runtime_error("schedule produced no MQTT commands");
    }
}

void SchedulerService::UpdateConfirmation(
    std::string_view key,
    std::string_view payload) {
    if (!active_) {
        return;
    }
    for (ExpectedFact& expected : active_->expected) {
        if (expected.key == key && expected.expected == payload) {
            expected.confirmed = true;
        }
    }
}

void SchedulerService::CompleteActiveIfReady() {
    if (!active_ || active_->expected.empty()) {
        return;
    }
    const bool complete = std::all_of(
        active_->expected.begin(), active_->expected.end(),
        [](const ExpectedFact& fact) { return fact.confirmed; });
    if (!complete) {
        return;
    }
    PublishRunResult(*active_, true, "completed", "All factual values confirmed");
    active_.reset();
    PublishStatus("ready", "Schedule completed");
}

void SchedulerService::FailActive(
    std::string_view state,
    std::string_view message) {
    if (!active_) {
        return;
    }
    PublishRunResult(*active_, false, state, message);
    active_.reset();
    PublishStatus("warning", message);
}

void SchedulerService::LoadState() {
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
        if (IsSafeId(id) && minuteKey.size() == 16U) {
            lastAutomaticMinute_[id] = minuteKey;
        }
    }
}

void SchedulerService::SaveState() const {
    std::ostringstream output;
    for (const auto& [id, minuteKey] : lastAutomaticMinute_) {
        output << id << '\t' << minuteKey << '\n';
    }
    WriteStateAtomically(paths_.state, output.str());
}

void SchedulerService::PruneState() {
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
        } else {
            ++iterator;
        }
    }
    if (changed) {
        SaveState();
    }
}

void SchedulerService::PublishStatus(
    std::string_view state,
    std::string_view message) {
    std::string payload =
        "{\"state\":\"" + JsonEscape(state) + "\"" +
        ",\"revision\":" + std::to_string(schedules_.revision) +
        ",\"schedules\":" + std::to_string(schedules_.schedules.size()) +
        ",\"enabled\":" + std::to_string(EnabledCount(schedules_)) +
        ",\"queued\":" + std::to_string(runQueue_.size()) +
        ",\"active\":" + std::string(active_ ? "true" : "false");
    if (active_) {
        payload += ",\"activeSchedule\":\"" +
            JsonEscape(active_->run.schedule.id) + "\"";
    }
    if (!message.empty()) {
        payload += ",\"message\":\"" + JsonEscape(message) + "\"";
    }
    payload += '}';
    client_.Publish(StatusTopic, payload, true);
}

void SchedulerService::PublishRunResult(
    const ActiveRun& run,
    bool success,
    std::string_view state,
    std::string_view message) const {
    const std::size_t confirmed = static_cast<std::size_t>(std::count_if(
        run.expected.begin(), run.expected.end(),
        [](const ExpectedFact& fact) { return fact.confirmed; }));
    const std::string payload =
        std::string("{\"success\":") + (success ? "true" : "false") +
        ",\"scheduleId\":\"" + JsonEscape(run.run.schedule.id) + "\"" +
        ",\"state\":\"" + JsonEscape(state) + "\"" +
        ",\"source\":\"" + JsonEscape(run.run.source) + "\"" +
        ",\"commands\":" + std::to_string(run.commandCount) +
        ",\"confirmed\":" + std::to_string(confirmed) +
        ",\"message\":\"" + JsonEscape(message) + "\"}";
    client_.Publish(ResultTopic(run.run.schedule.id), payload, false);
}

bool SchedulerService::IsDue(
    const ScheduleEntry& schedule,
    const SchedulerLocalMinute& minute) {
    if (schedule.time != minute.TimeText()) {
        return false;
    }
    if (schedule.kind == ScheduleKind::Once) {
        return schedule.date == minute.DateText();
    }
    return std::find(schedule.days.begin(), schedule.days.end(), minute.weekday) !=
        schedule.days.end();
}

std::string SchedulerService::CommandTopic(
    const ScheduleTarget& target,
    std::string_view control) {
    return "/devices/Fan-" + std::to_string(target.bus) + "_" +
        std::to_string(target.address) + "/controls/" +
        std::string(control) + "/on1";
}

std::string SchedulerService::FactKey(
    const ScheduleTarget& target,
    std::string_view control) {
    return "/devices/Fan-" + std::to_string(target.bus) + "_" +
        std::to_string(target.address) + "/controls/" + std::string(control);
}

int RunSchedulerDaemon(
    const SchedulerPaths& paths,
    std::ostream& output,
    std::ostream& errors) {
    if (!mdv::MosquittoMqttClient::IsSupported()) {
        errors << "SCHEDULER_ERROR: libmosquitto support is not available in this build\n";
        return 1;
    }
    try {
        StopRequested.store(false);
        std::signal(SIGINT, HandleStopSignal);
        std::signal(SIGTERM, HandleStopSignal);

        mdv::MosquittoMqttClient client(SchedulerMqttOptionsFromEnvironment());
        SystemSchedulerClock clock;
        SchedulerService service(client, paths, clock);
        service.Start();
        client.Start();
        output << "SCHEDULER_STARTED schedules=" << paths.schedules.string() << '\n';

        while (!StopRequested.load()) {
            while (service.ProcessOne().has_value()) {
            }
            service.Tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        client.Stop();
        output << "SCHEDULER_STOPPED\n";
        return 0;
    } catch (const std::exception& error) {
        errors << "SCHEDULER_ERROR: " << error.what() << '\n';
        return 1;
    }
}

} // namespace mdvwb
