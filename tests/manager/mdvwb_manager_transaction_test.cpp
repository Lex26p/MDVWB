#include "mdvwb_manager_mqtt.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        static unsigned long long counter = 0;
        const auto token = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-manager-transaction-test-" + std::to_string(token) + "-" +
             std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void WriteFile(const std::filesystem::path& path, std::string_view content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write transaction test fixture");
    }
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read transaction test fixture");
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

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
        publications.push_back(
            {std::string(topic), std::string(payload), retained});
    }

    void Inject(std::string topic, std::string payload, bool retained = false)
    {
        Require(static_cast<bool>(handler_), "message handler is not installed");
        handler_({std::move(topic), std::move(payload), retained});
    }

    [[nodiscard]] const mdv::MqttPublication* Last(
        std::string_view topic) const
    {
        for (auto iterator = publications.rbegin();
             iterator != publications.rend(); ++iterator) {
            if (iterator->topic == topic) {
                return &*iterator;
            }
        }
        return nullptr;
    }

    MessageHandler handler_;
    std::vector<std::string> subscriptions;
    std::vector<mdv::MqttPublication> publications;
};

int ParseServiceBusId(const std::string& service)
{
    constexpr std::string_view Prefix = "mdvwb@";
    constexpr std::string_view Suffix = ".service";
    Require(service.rfind(Prefix, 0U) == 0U && service.ends_with(Suffix),
        "unexpected service name in transaction test");
    const std::string number = service.substr(
        Prefix.size(), service.size() - Prefix.size() - Suffix.size());
    return std::stoi(number);
}

class StatefulRunner final : public mdvwb::CommandRunner {
public:
    int Run(const std::vector<std::string>& arguments) override
    {
        commands.push_back(arguments);
        Require(arguments.size() >= 3U, "unexpected short systemctl command");
        const std::string& action = arguments[1];
        const int busId = ParseServiceBusId(arguments.back());
        mdvwb::BusServiceStatus& status = states[busId];

        if (action == "is-active") {
            return status.active ? 0 : 3;
        }
        if (action == "is-enabled") {
            return status.enabled ? 0 : 1;
        }

        const auto failed = std::find(
            failedCommands.begin(), failedCommands.end(), arguments);
        if (failed != failedCommands.end()) {
            failedCommands.erase(failed);
            return 5;
        }

        const bool now = arguments.size() >= 4U && arguments[2] == "--now";
        if (action == "enable") {
            status.enabled = true;
            if (now) {
                status.active = true;
            }
        } else if (action == "disable") {
            status.enabled = false;
            if (now) {
                status.active = false;
            }
        } else if (action == "start" || action == "restart") {
            status.active = true;
        } else if (action == "stop") {
            status.active = false;
        } else {
            throw std::runtime_error("unexpected systemctl action in transaction test");
        }
        return 0;
    }

    void ResetMutations()
    {
        commands.clear();
        failedCommands.clear();
    }

    std::map<int, mdvwb::BusServiceStatus> states;
    std::vector<std::vector<std::string>> commands;
    std::vector<std::vector<std::string>> failedCommands;
};


std::string DescribeCommands(const StatefulRunner& runner)
{
    std::string result;
    for (const auto& command : runner.commands) {
        if (!result.empty()) {
            result += " |";
        }
        for (const auto& part : command) {
            result += " " + part;
        }
    }
    return result;
}

struct Fixture {
    TemporaryDirectory temporary;
    std::filesystem::path config = temporary.Path() / "etc/mdvwb/buses.json";
    std::filesystem::path dashboard = temporary.Path() / "etc/mdvwb/dashboard.json";
    std::filesystem::path schedules = temporary.Path() / "etc/mdvwb/schedules.json";
    std::filesystem::path assets = temporary.Path() / "assets";
    mdvwb::ServiceSyncPaths paths;
    FakeMqttClient client;
    StatefulRunner runner;

    Fixture()
    {
        paths.defaultDirectory = temporary.Path() / "defaults";
        paths.environmentTemplate = temporary.Path() / "mdvwb.env";
        paths.systemctlProgram = "fake-systemctl";

        WriteFile(
            paths.environmentTemplate,
            "MDVWB_ADDRESSES=\"1\"\n"
            "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
            "MDVWB_BUS=\"1\"\n"
            "MDVWB_MASTER_ID=\"0\"\n"
            "MDVWB_PERIOD_MS=\"150\"\n");

        WriteFile(config, R"json({
  "version": 1,
  "revision": 3,
  "buses": [
    {"id": 1, "enabled": true, "port": "/dev/ttyRS485-1", "addresses": [1]},
    {"id": 2, "enabled": false, "port": "/dev/ttyRS485-2", "addresses": [2]}
  ]
})json");

        WriteFile(dashboard, R"json({
  "version": 2,
  "revision": 1,
  "defaultPanel": "main",
  "panels": [{
    "id": "main",
    "title": "Main",
    "background": {
      "file": "", "naturalWidth": 0, "naturalHeight": 0,
      "defaultScale": 1, "fit": "contain"
    },
    "fans": [{
      "id": "fan-1-1", "number": 1, "bus": 1, "address": 1,
      "label": "Fan 1", "x": 0.5, "y": 0.5,
      "markerScale": 1, "rotation": 0, "visible": true
    }]
  }]
})json");

        WriteFile(schedules, R"json({
  "version": 1,
  "revision": 1,
  "schedules": [{
    "id": "workday", "name": "Workday", "enabled": true,
    "panelId": "main", "kind": "weekly", "days": [1],
    "date": "", "time": "08:00",
    "targets": [{"bus": 1, "address": 1}],
    "actions": {"power": true}
  }]
})json");

        const mdvwb::BusesConfig initial = mdvwb::LoadBusesConfig(config);
        mdvwb::ApplyServiceSyncPlan(
            mdvwb::BuildServiceSyncPlan(initial, paths), paths, runner);
        runner.ResetMutations();
    }

    [[nodiscard]] mdvwb::ManagerMqttService CreateService()
    {
        return mdvwb::ManagerMqttService(
            client,
            config,
            paths,
            runner,
            nullptr,
            dashboard,
            assets,
            schedules);
    }
};

constexpr std::string_view ChangedConfiguration = R"json({
  "version": 1,
  "revision": 3,
  "buses": [
    {"id": 1, "enabled": true, "port": "/dev/ttyUSB7", "addresses": [1]},
    {"id": 3, "enabled": true, "port": "/dev/ttyRS485-3", "addresses": [3]}
  ]
})json";

void TestApplyFailureRestoresPreviousConfiguration()
{
    Fixture fixture;
    auto service = fixture.CreateService();
    service.Start();
    fixture.client.publications.clear();

    // Fail only the apply-time start of the newly added bus. All commands
    // issued by the rollback remain successful, independently of plan order.
    fixture.runner.failedCommands.push_back(
        {"fake-systemctl", "enable", "--now", "mdvwb@3.service"});
    fixture.client.Inject(
        mdvwb::ManagerMqttService::ConfigSetTopic,
        std::string(ChangedConfiguration));

    const auto result = service.ProcessOne();
    if (!result.has_value() || result->success || result->saved) {
        throw std::runtime_error(
            "failed service apply was reported incorrectly: success=" +
            std::string(result.has_value() && result->success ? "true" : "false") +
            ", saved=" +
            std::string(result.has_value() && result->saved ? "true" : "false") +
            ", message=" +
            (result.has_value() ? result->message : std::string("<no result>")) +
            ", commands=" + DescribeCommands(fixture.runner));
    }
    Require(result->message.find("previous configuration restored") !=
            std::string::npos,
        "successful rollback was not reported");

    const std::string saved = ReadFile(fixture.config);
    Require(saved.find("\"revision\": 3") != std::string::npos &&
            saved.find("/dev/ttyRS485-1") != std::string::npos &&
            saved.find("/dev/ttyUSB7") == std::string::npos,
        "buses.json was not restored after apply failure");
    Require(!std::filesystem::exists(fixture.config.string() + ".pending"),
        "staged buses configuration was left behind");

    const std::string bus1 = ReadFile(fixture.paths.defaultDirectory / "mdvwb-1");
    const std::string bus2 = ReadFile(fixture.paths.defaultDirectory / "mdvwb-2");
    Require(bus1.find("/dev/ttyRS485-1") != std::string::npos,
        "bus 1 environment was not rolled back");
    Require(bus2.find("/dev/ttyRS485-2") != std::string::npos,
        "removed bus 2 environment was not restored");
    Require(!std::filesystem::exists(
                fixture.paths.defaultDirectory / "mdvwb-3"),
        "new bus 3 environment survived rollback");

    Require(fixture.runner.states[1].enabled && fixture.runner.states[1].active,
        "bus 1 service state was not restored");
    Require(!fixture.runner.states[2].enabled && !fixture.runner.states[2].active,
        "bus 2 service state was not restored");
    Require(!fixture.runner.states[3].enabled && !fixture.runner.states[3].active,
        "new bus 3 service was not stopped by rollback");

    const auto* current = fixture.client.Last(
        mdvwb::ManagerMqttService::ConfigTopic);
    Require(current != nullptr && current->retained &&
            current->payload.find("/dev/ttyRS485-1") != std::string::npos &&
            current->payload.find("/dev/ttyUSB7") == std::string::npos,
        "previous retained configuration was not republished");
    const auto* operation = fixture.client.Last(
        mdvwb::ManagerMqttService::ConfigResultTopic);
    Require(operation != nullptr && !operation->retained &&
            operation->payload.find("\"success\":false") != std::string::npos &&
            operation->payload.find("\"saved\":false") != std::string::npos,
        "rollback result does not report unsaved failure");
}

void TestRollbackFailurePublishesDegradedStatus()
{
    Fixture fixture;
    auto service = fixture.CreateService();
    service.Start();
    fixture.client.publications.clear();

    // Fail the apply-time start of bus 3 and then fail the rollback-time
    // stop of that same new service. Exact commands avoid depending on the
    // number or order of unrelated systemd operations.
    fixture.runner.failedCommands.push_back(
        {"fake-systemctl", "enable", "--now", "mdvwb@3.service"});
    fixture.runner.failedCommands.push_back(
        {"fake-systemctl", "disable", "--now", "mdvwb@3.service"});
    fixture.client.Inject(
        mdvwb::ManagerMqttService::ConfigSetTopic,
        std::string(ChangedConfiguration));

    const auto result = service.ProcessOne();
    Require(result.has_value() && !result->success && result->saved,
        "degraded submitted configuration was not reported as saved");
    Require(result->message.find("rollback is incomplete") != std::string::npos,
        "incomplete rollback was not reported explicitly");
    const std::string saved = ReadFile(fixture.config);
    Require(saved.find("/dev/ttyUSB7") != std::string::npos &&
            saved.find("\"revision\": 4") != std::string::npos,
        "validated submitted buses.json was not kept as degraded recovery target");
    Require(!std::filesystem::exists(fixture.config.string() + ".pending"),
        "pending file survived degraded rollback");

    const auto* current = fixture.client.Last(
        mdvwb::ManagerMqttService::ConfigTopic);
    Require(current != nullptr && current->retained &&
            current->payload.find("/dev/ttyUSB7") != std::string::npos,
        "degraded configuration was not republished retained");

    const auto* status = fixture.client.Last(
        mdvwb::ManagerMqttService::StatusTopic);
    Require(status != nullptr && status->retained &&
            status->payload.find("\"state\":\"error\"") != std::string::npos &&
            status->payload.find("rollback is incomplete") != std::string::npos,
        "degraded rollback did not publish retained error status");
}

}  // namespace

int main()
{
    try {
        TestApplyFailureRestoresPreviousConfiguration();
        TestRollbackFailurePublishesDegradedStatus();
        std::cout << "MDVWB manager transaction tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB manager transaction tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
