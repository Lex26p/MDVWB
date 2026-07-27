#include "mdvwb_manager_mqtt.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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
            ("mdvwb-manager-revision-test-" + std::to_string(token) + "-" +
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
             iterator != publications.rend();
             ++iterator) {
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

class RecordingRunner final : public mdvwb::CommandRunner {
public:
    int Run(const std::vector<std::string>& arguments) override
    {
        commands.push_back(arguments);
        if (arguments.size() >= 2U && arguments[1] == "is-active") {
            return 3;
        }
        if (arguments.size() >= 2U && arguments[1] == "is-enabled") {
            return 1;
        }
        return 0;
    }

    std::vector<std::vector<std::string>> commands;
};

void WriteFile(const std::filesystem::path& path, std::string_view content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write test file: " + path.string());
    }
}

std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read test file: " + path.string());
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

struct Fixture {
    TemporaryDirectory temporary;
    std::filesystem::path config =
        temporary.Path() / "etc/mdvwb/buses.json";
    std::filesystem::path dashboard =
        temporary.Path() / "etc/mdvwb/dashboard.json";
    std::filesystem::path schedules =
        temporary.Path() / "etc/mdvwb/schedules.json";
    std::filesystem::path assets = temporary.Path() / "assets";
    mdvwb::ServiceSyncPaths paths;
    FakeMqttClient client;
    RecordingRunner runner;

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
    {
      "id": 1,
      "enabled": true,
      "port": "/dev/ttyRS485-1",
      "addresses": [1]
    }
  ]
})json");

        WriteFile(dashboard, R"json({
  "version": 2,
  "revision": 1,
  "defaultPanel": "main",
  "panels": [
    {
      "id": "main",
      "title": "Main",
      "background": {
        "file": "",
        "naturalWidth": 0,
        "naturalHeight": 0,
        "defaultScale": 1,
        "fit": "contain"
      },
      "fans": [
        {
          "id": "fan-1-1",
          "number": 1,
          "bus": 1,
          "address": 1,
          "label": "Fan 1",
          "x": 0.5,
          "y": 0.5,
          "markerScale": 1,
          "rotation": 0,
          "visible": true
        }
      ]
    }
  ]
})json");

        WriteFile(schedules, R"json({
  "version": 1,
  "revision": 1,
  "schedules": [
    {
      "id": "workday",
      "name": "Workday",
      "enabled": true,
      "panelId": "main",
      "kind": "weekly",
      "days": [1],
      "date": "",
      "time": "08:00",
      "targets": [{"bus": 1, "address": 1}],
      "actions": {"power": true}
    }
  ]
})json");
    }

    mdvwb::ManagerMqttService CreateService()
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

void TestStaleRevisionIsRejected()
{
    Fixture fixture;
    auto service = fixture.CreateService();
    service.Start();
    const std::size_t commandCountBefore = fixture.runner.commands.size();

    fixture.client.Inject(
        mdvwb::ManagerMqttService::ConfigSetTopic,
        R"json({"version":1,"revision":2,"buses":[
          {"id":1,"enabled":true,"port":"/dev/ttyUSB7","addresses":[1]}
        ]})json");

    const auto result = service.ProcessOne();
    Require(result.has_value() && !result->success && !result->saved,
        "stale configuration revision should be rejected");
    Require(result->message.find("revision conflict") != std::string::npos,
        "revision rejection should explain the conflict");
    Require(fixture.runner.commands.size() == commandCountBefore,
        "revision conflict must not synchronize services");

    const std::string saved = ReadFile(fixture.config);
    Require(saved.find("\"revision\": 3") != std::string::npos,
        "revision conflict changed the saved revision");
    Require(saved.find("/dev/ttyRS485-1") != std::string::npos,
        "revision conflict changed the saved bus");

    const auto* current = fixture.client.Last(
        mdvwb::ManagerMqttService::ConfigTopic);
    Require(current != nullptr && current->retained &&
            current->payload.find("\"revision\": 3") != std::string::npos,
        "current configuration was not republished after a conflict");
}

void TestNewBrokenReferencesAreRejected()
{
    Fixture fixture;
    auto service = fixture.CreateService();
    service.Start();
    const std::size_t commandCountBefore = fixture.runner.commands.size();

    fixture.client.Inject(
        mdvwb::ManagerMqttService::ConfigSetTopic,
        R"json({"version":1,"revision":3,"buses":[
          {"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[2]}
        ]})json");

    const auto result = service.ProcessOne();
    Require(result.has_value() && !result->success && !result->saved,
        "configuration that breaks references should be rejected");
    Require(result->message.find("would break references") != std::string::npos,
        "broken-reference rejection should explain the reason");
    Require(fixture.runner.commands.size() == commandCountBefore,
        "broken references must not synchronize services");

    const std::string saved = ReadFile(fixture.config);
    Require(saved.find("\"revision\": 3") != std::string::npos,
        "broken-reference rejection changed the revision");
    Require(saved.find("\"addresses\": [1]") != std::string::npos,
        "broken-reference rejection removed a referenced address");
}

void TestValidSaveIncrementsRevision()
{
    Fixture fixture;
    auto service = fixture.CreateService();
    service.Start();

    fixture.client.Inject(
        mdvwb::ManagerMqttService::ConfigSetTopic,
        R"json({"version":1,"revision":3,"buses":[
          {"id":1,"enabled":true,"port":"/dev/ttyUSB7","addresses":[1,2]}
        ]})json");

    const auto result = service.ProcessOne();
    Require(result.has_value() && result->success && result->saved,
        "valid configuration should be saved and applied");

    const std::string saved = ReadFile(fixture.config);
    Require(saved.find("\"revision\": 4") != std::string::npos,
        "successful save did not increment the revision");
    Require(saved.find("\"addresses\": [1, 2]") != std::string::npos,
        "successful save did not preserve the new addresses");
    Require(saved.find("/dev/ttyUSB7") != std::string::npos,
        "successful save did not preserve the new port");

    const auto* current = fixture.client.Last(
        mdvwb::ManagerMqttService::ConfigTopic);
    Require(current != nullptr && current->retained &&
            current->payload.find("\"revision\": 4") != std::string::npos,
        "incremented configuration was not published");

    const auto* operation = fixture.client.Last(
        mdvwb::ManagerMqttService::ConfigResultTopic);
    Require(operation != nullptr && !operation->retained &&
            operation->payload.find("\"success\":true") != std::string::npos,
        "successful save result was not published");
}

}  // namespace

int main()
{
    try {
        TestStaleRevisionIsRejected();
        TestNewBrokenReferencesAreRejected();
        TestValidSaveIncrementsRevision();
        std::cout << "MDVWB manager revision tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB manager revision tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
