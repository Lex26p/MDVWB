#include "mdvwb_manager_mqtt.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <mutex>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-manager-mqtt-test-" + std::to_string(token));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    const std::filesystem::path& Path() const { return path_; }
private:
    std::filesystem::path path_;
};

class FakeMqttClient final : public mdv::IMqttClient {
public:
    void SetMessageHandler(MessageHandler handler) override {
        handler_ = std::move(handler);
    }
    void Subscribe(std::string_view topicFilter) override {
        subscriptions.emplace_back(topicFilter);
    }
    void Publish(
        std::string_view topic,
        std::string_view payload,
        bool retained) override {
        publications.push_back({std::string(topic), std::string(payload), retained});
    }
    void Inject(std::string topic, std::string payload, bool retained = false) {
        Require(static_cast<bool>(handler_), "message handler is not installed");
        handler_({std::move(topic), std::move(payload), retained});
    }

    MessageHandler handler_;
    std::vector<std::string> subscriptions;
    std::vector<mdv::MqttPublication> publications;
};


class FakeDiscoveryRunner final : public mdvwb::DiscoveryRunner {
public:
    mdvwb::DiscoveryExecutionResult Run(
        std::string_view port,
        int masterId,
        int periodMilliseconds,
        int responseTimeoutMilliseconds) override {
        ++calls;
        lastPort = std::string(port);
        lastMasterId = masterId;
        lastPeriodMilliseconds = periodMilliseconds;
        lastResponseTimeoutMilliseconds = responseTimeoutMilliseconds;
        return result;
    }

    int calls = 0;
    std::string lastPort;
    int lastMasterId = -1;
    int lastPeriodMilliseconds = -1;
    int lastResponseTimeoutMilliseconds = -1;
    mdvwb::DiscoveryExecutionResult result{
        true, 0, {1, 3, 18}, "FOUND_ADDRESSES=1,3,18\n", "Discovery completed"};
};

class RecordingRunner final : public mdvwb::CommandRunner {
public:
    int Run(const std::vector<std::string>& arguments) override {
        commands.push_back(arguments);
        if (arguments.size() >= 2U && arguments[1] == "is-active") {
            return active ? 0 : 3;
        }
        if (arguments.size() >= 2U && arguments[1] == "is-enabled") {
            return enabled ? 0 : 1;
        }
        return returnCode;
    }
    int returnCode = 0;
    bool active = true;
    bool enabled = true;
    std::vector<std::vector<std::string>> commands;
};

void WriteFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write test file");
    }
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

std::string PngHeader(int width, int height) {
    std::string data(32, '\0');
    const unsigned char signature[] = {
        0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    for (std::size_t index = 0; index < 8U; ++index) {
        data[index] = static_cast<char>(signature[index]);
    }
    data[11] = 13;
    data.replace(12, 4, "IHDR");
    data[16] = static_cast<char>((width >> 24) & 0xff);
    data[17] = static_cast<char>((width >> 16) & 0xff);
    data[18] = static_cast<char>((width >> 8) & 0xff);
    data[19] = static_cast<char>(width & 0xff);
    data[20] = static_cast<char>((height >> 24) & 0xff);
    data[21] = static_cast<char>((height >> 16) & 0xff);
    data[22] = static_cast<char>((height >> 8) & 0xff);
    data[23] = static_cast<char>(height & 0xff);
    return data;
}

const mdv::MqttPublication* LastPublication(
    const FakeMqttClient& client,
    std::string_view topic) {
    for (auto iterator = client.publications.rbegin();
         iterator != client.publications.rend(); ++iterator) {
        if (iterator->topic == topic) {
            return &*iterator;
        }
    }
    return nullptr;
}

bool HasCommand(
    const RecordingRunner& runner,
    const std::vector<std::string>& expected) {
    for (const auto& command : runner.commands) {
        if (command == expected) {
            return true;
        }
    }
    return false;
}

struct Fixture {
    TemporaryDirectory temporary;
    std::filesystem::path config = temporary.Path() / "etc/mdvwb/buses.json";
    std::filesystem::path dashboard = temporary.Path() / "etc/mdvwb/dashboard.json";
    std::filesystem::path schedules = temporary.Path() / "etc/mdvwb/schedules.json";
    mdvwb::ServiceSyncPaths paths;
    FakeMqttClient client;
    RecordingRunner runner;
    FakeDiscoveryRunner discovery;

    Fixture() {
        paths.defaultDirectory = temporary.Path() / "defaults";
        paths.environmentTemplate = temporary.Path() / "mdvwb.env";
        paths.systemctlProgram = "fake-systemctl";
        WriteFile(paths.environmentTemplate,
            "MDVWB_ADDRESSES=\"1\"\n"
            "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
            "MDVWB_BUS=\"1\"\n"
            "MDVWB_MASTER_ID=\"0\"\n");
        WriteFile(config, R"json({
          "version": 1,
          "buses": [
            {"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1]},
            {"id":2,"enabled":false,"port":"/dev/ttyUSB0","addresses":[]}
          ]
        })json");
    }
};

void TestStartPublishesConfigurationAndBusStatuses() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();

    Require(fixture.client.subscriptions.size() == 13,
            "manager must create thirteen subscriptions");
    Require(fixture.client.subscriptions[0] == mdvwb::ManagerMqttService::ConfigSetTopic,
            "manager subscribed to wrong configuration topic");
    Require(fixture.client.subscriptions[1] == mdvwb::ManagerMqttService::DashboardConfigSetTopic,
            "manager subscribed to wrong dashboard configuration topic");
    Require(fixture.client.subscriptions[2] == mdvwb::ManagerMqttService::SchedulesConfigSetTopic,
            "manager subscribed to wrong schedules configuration topic");
    Require(fixture.client.subscriptions[3] == mdvwb::ManagerMqttService::ScheduleRunFilter,
            "manager subscribed to wrong schedule run filter");
    Require(fixture.client.subscriptions[4] == mdvwb::ManagerMqttService::BackgroundUploadStartTopic,
            "manager subscribed to wrong upload start topic");
    Require(fixture.client.subscriptions[5] == mdvwb::ManagerMqttService::BackgroundUploadChunkFilter,
            "manager subscribed to wrong upload chunk filter");
    Require(fixture.client.subscriptions[6] == mdvwb::ManagerMqttService::BackgroundUploadFinishFilter,
            "manager subscribed to wrong upload finish filter");
    Require(fixture.client.subscriptions[7] == mdvwb::ManagerMqttService::BackgroundUploadCancelFilter,
            "manager subscribed to wrong upload cancel filter");
    Require(fixture.client.subscriptions[8] == mdvwb::ManagerMqttService::BusStartFilter,
            "manager subscribed to wrong start filter");
    Require(fixture.client.subscriptions[9] == mdvwb::ManagerMqttService::BusStopFilter,
            "manager subscribed to wrong stop filter");
    Require(fixture.client.subscriptions[10] == mdvwb::ManagerMqttService::BusRestartFilter,
            "manager subscribed to wrong restart filter");
    Require(fixture.client.subscriptions[11] == mdvwb::ManagerMqttService::BusStatusGetFilter,
            "manager subscribed to wrong status filter");
    Require(fixture.client.subscriptions[12] == mdvwb::ManagerMqttService::BusDiscoveryStartFilter,
            "manager subscribed to wrong discovery filter");

    const auto* config = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::ConfigTopic);
    Require(config != nullptr && config->retained,
            "current configuration must be retained");

    const auto* status = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::StatusTopic);
    Require(status != nullptr && status->retained,
            "manager status must be retained");
    Require(status->payload.find("\"state\":\"ready\"") != std::string::npos,
            "manager did not publish ready state");

    const auto* busStatus = LastPublication(fixture.client, "/mdvwb/buses/1/status");
    Require(busStatus != nullptr && busStatus->retained,
            "bus status must be retained");
    Require(busStatus->payload.find("\"service\":\"active\"") != std::string::npos,
            "active service state is missing");
    Require(busStatus->payload.find("\"port\":\"/dev/ttyRS485-1\"") != std::string::npos,
            "bus port is missing from status");

    Require(std::filesystem::exists(fixture.dashboard),
            "default dashboard configuration was not created");
    const auto* dashboardConfig = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::DashboardConfigTopic);
    Require(dashboardConfig != nullptr && dashboardConfig->retained,
            "current dashboard configuration must be retained");
    Require(dashboardConfig->payload.find("\"revision\": 0") != std::string::npos,
            "default dashboard revision is wrong");
    const auto* dashboardStatus = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::DashboardStatusTopic);
    Require(dashboardStatus != nullptr && dashboardStatus->retained,
            "dashboard status must be retained");
    Require(dashboardStatus->payload.find("\"state\":\"ready\"") != std::string::npos,
            "dashboard did not publish ready state");
    const auto* uploadStatus = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::BackgroundUploadStatusTopic);
    Require(uploadStatus != nullptr && uploadStatus->retained &&
                uploadStatus->payload.find("\"state\":\"idle\"") != std::string::npos,
            "background upload did not publish idle state");

    Require(std::filesystem::exists(fixture.schedules),
            "default schedules configuration was not created");
    const auto* schedulesConfig = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::SchedulesConfigTopic);
    Require(schedulesConfig != nullptr && schedulesConfig->retained &&
                schedulesConfig->payload.find("\"revision\": 0") != std::string::npos,
            "current schedules configuration must be retained");
    const auto* schedulesStatus = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::SchedulesStatusTopic);
    Require(schedulesStatus != nullptr && schedulesStatus->retained &&
                schedulesStatus->payload.find("\"state\":\"ready\"") != std::string::npos,
            "schedules did not publish ready state");
}

void TestValidConfigurationIsSavedAndApplied() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();

    fixture.client.Inject(
        mdvwb::ManagerMqttService::ConfigSetTopic,
        R"json({"version":1,"buses":[
          {"id":2,"enabled":false,"port":"/dev/ttyUSB0","addresses":[]},
          {"id":1,"enabled":true,"port":"/dev/ttyRS485-7","addresses":[3,1,2]}
        ]})json");

    const auto result = service.ProcessOne();
    Require(result.has_value() && result->success && result->saved,
            "valid configuration was not applied");
    Require(service.PendingCount() == 0, "processed command remained queued");

    const std::string saved = ReadFile(fixture.config);
    Require(saved.find("\"id\": 1") < saved.find("\"id\": 2"),
            "saved buses are not normalized");
    Require(saved.find("\"addresses\": [1, 2, 3]") != std::string::npos,
            "saved addresses are not normalized");
    Require(std::filesystem::exists(fixture.paths.defaultDirectory / "mdvwb-1"),
            "bus environment was not generated");

    const auto* publication = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::ConfigResultTopic);
    Require(publication != nullptr && !publication->retained,
            "operation result must not be retained");
    Require(publication->payload.find("\"success\":true") != std::string::npos,
            "success result is missing");
}

void TestBusStartStopRestartAndStatus() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    fixture.runner.commands.clear();

    fixture.client.Inject("/mdvwb/buses/1/start", "1");
    auto result = service.ProcessOne();
    Require(result.has_value() && result->success && result->busId == 1 &&
                result->command == "start",
            "start command failed");
    Require(HasCommand(fixture.runner,
                {"fake-systemctl", "start", "mdvwb@1.service"}),
            "start systemctl command is wrong");

    fixture.client.Inject("/mdvwb/buses/1/stop", "1");
    result = service.ProcessOne();
    Require(result.has_value() && result->success && result->command == "stop",
            "stop command failed");
    Require(HasCommand(fixture.runner,
                {"fake-systemctl", "stop", "mdvwb@1.service"}),
            "stop systemctl command is wrong");

    fixture.client.Inject("/mdvwb/buses/1/restart", "1");
    result = service.ProcessOne();
    Require(result.has_value() && result->success && result->command == "restart",
            "restart command failed");
    Require(HasCommand(fixture.runner,
                {"fake-systemctl", "restart", "mdvwb@1.service"}),
            "restart systemctl command is wrong");

    const std::size_t beforeStatus = fixture.runner.commands.size();
    fixture.client.Inject("/mdvwb/buses/1/status/get", "1");
    result = service.ProcessOne();
    Require(result.has_value() && result->success && result->command == "status",
            "status command failed");
    Require(fixture.runner.commands.size() == beforeStatus + 2U,
            "status request must only query active and enabled states");

    const auto* operation = LastPublication(fixture.client, "/mdvwb/buses/1/result");
    Require(operation != nullptr && !operation->retained,
            "bus operation result must not be retained");
    Require(operation->payload.find("\"success\":true") != std::string::npos,
            "bus operation success is missing");
}

void TestUnknownDisabledAndRetainedBusCommandsAreRejected() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    fixture.runner.commands.clear();

    fixture.client.Inject("/mdvwb/buses/99/start", "1");
    auto result = service.ProcessOne();
    Require(result.has_value() && !result->success,
            "unknown bus command was accepted");
    Require(fixture.runner.commands.empty(),
            "unknown bus executed systemctl");

    fixture.client.Inject("/mdvwb/buses/2/start", "1");
    result = service.ProcessOne();
    Require(result.has_value() && !result->success,
            "disabled bus start was accepted");
    Require(!HasCommand(fixture.runner,
                {"fake-systemctl", "start", "mdvwb@2.service"}),
            "disabled bus was started");

    fixture.client.Inject("/mdvwb/buses/1/restart", "1", true);
    result = service.ProcessOne();
    Require(result.has_value() && !result->success,
            "retained bus command was accepted");
    Require(!HasCommand(fixture.runner,
                {"fake-systemctl", "restart", "mdvwb@1.service"}),
            "retained bus command executed systemctl");
}


void TestDiscoveryStopsOnlySelectedBusAndPublishesResult() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    fixture.runner.commands.clear();

    fixture.client.Inject("/mdvwb/buses/1/discovery/start", "1");
    const auto result = service.ProcessOne();
    Require(result.has_value() && result->success && result->busId == 1 &&
                result->command == "discovery",
            "discovery command failed");
    Require(HasCommand(fixture.runner,
                {"fake-systemctl", "stop", "mdvwb@1.service"}),
            "discovery did not stop selected active bus");
    Require(!HasCommand(fixture.runner,
                {"fake-systemctl", "stop", "mdvwb@2.service"}),
            "discovery stopped another bus");
    Require(fixture.discovery.calls == 1,
            "discovery runner was not called exactly once");
    Require(fixture.discovery.lastPort == "/dev/ttyRS485-1",
            "discovery used wrong port");
    Require(fixture.discovery.lastMasterId == 0 &&
                fixture.discovery.lastPeriodMilliseconds == 150 &&
                fixture.discovery.lastResponseTimeoutMilliseconds == 130,
            "discovery used wrong protocol timing");

    const auto* status = LastPublication(
        fixture.client, "/mdvwb/buses/1/discovery/status");
    Require(status != nullptr && status->retained,
            "discovery status must be retained");
    Require(status->payload.find("\"state\":\"completed\"") != std::string::npos &&
                status->payload.find("\"found\":3") != std::string::npos,
            "completed discovery status is wrong");

    const auto* discoveryResult = LastPublication(
        fixture.client, "/mdvwb/buses/1/discovery/result");
    Require(discoveryResult != nullptr && discoveryResult->retained,
            "discovery result must be retained");
    Require(discoveryResult->payload.find("\"addresses\":[1,3,18]") != std::string::npos,
            "discovery addresses are wrong");
}

void TestDiscoveryRejectsUnknownRetainedAndFailedRuns() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    fixture.runner.commands.clear();

    fixture.client.Inject("/mdvwb/buses/99/discovery/start", "1");
    auto result = service.ProcessOne();
    Require(result.has_value() && !result->success,
            "unknown bus discovery was accepted");
    Require(fixture.discovery.calls == 0,
            "unknown bus invoked discovery runner");

    fixture.client.Inject("/mdvwb/buses/1/discovery/start", "1", true);
    result = service.ProcessOne();
    Require(result.has_value() && !result->success,
            "retained discovery command was accepted");
    Require(fixture.discovery.calls == 0,
            "retained command invoked discovery runner");

    fixture.discovery.result = {
        false, 2, {}, "open failed\n", "Cannot open serial port"};
    fixture.client.Inject("/mdvwb/buses/1/discovery/start", "1");
    result = service.ProcessOne();
    Require(result.has_value() && !result->success,
            "failed discovery was reported as success");
    const auto* status = LastPublication(
        fixture.client, "/mdvwb/buses/1/discovery/status");
    Require(status != nullptr &&
                status->payload.find("\"state\":\"error\"") != std::string::npos,
            "failed discovery did not publish error status");
}

void TestRemovedDevicesClearRetainedTopics() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    fixture.client.publications.clear();

    fixture.client.Inject(
        mdvwb::ManagerMqttService::ConfigSetTopic,
        R"json({"version":1,"buses":[
          {"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[2]}
        ]})json");
    const auto result = service.ProcessOne();
    Require(result.has_value() && result->saved,
            "changed configuration was not saved");

    const auto* deviceName = LastPublication(
        fixture.client, "/devices/Fan-1_1/meta/name");
    Require(deviceName != nullptr && deviceName->retained && deviceName->payload.empty(),
            "removed device name topic was not cleared");
    const auto* state = LastPublication(
        fixture.client, "/devices/Fan-1_1/controls/Power");
    Require(state != nullptr && state->retained && state->payload.empty(),
            "removed device state topic was not cleared");
    const auto* removedBus = LastPublication(
        fixture.client, "/devices/sist-2/meta/name");
    Require(removedBus != nullptr && removedBus->retained && removedBus->payload.empty(),
            "removed bus system device was not cleared");
}

void TestDashboardConfigurationIsSavedWithRevisionAndWarnings() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    fixture.client.publications.clear();

    fixture.client.Inject(
        mdvwb::ManagerMqttService::DashboardConfigSetTopic,
        R"json({
          "version":1,
          "revision":0,
          "title":"Главная панель",
          "background":{
            "file":"","naturalWidth":0,"naturalHeight":0,
            "defaultScale":1,"fit":"contain"
          },
          "fans":[
            {"id":"fan-1-1","bus":1,"address":1,"label":"Кабинет 1",
             "x":0.25,"y":0.5,"markerScale":1,"rotation":0,"visible":true},
            {"id":"fan-2-18","bus":2,"address":18,"label":"Переговорная",
             "x":0.75,"y":0.5,"markerScale":1.2,"rotation":15,"visible":true}
          ]
        })json");

    const auto result = service.ProcessOne();
    Require(result.has_value() && result->success && result->saved &&
                result->command == "dashboard-config",
            "valid dashboard configuration was not saved");
    const std::string saved = ReadFile(fixture.dashboard);
    Require(saved.find("\"revision\": 1") != std::string::npos,
            "manager did not increment dashboard revision");
    Require(saved.find("\"number\": 1") != std::string::npos &&
                saved.find("\"number\": 2") != std::string::npos,
            "manager did not migrate user-facing fan numbers");
    Require(saved.find("\"id\": \"fan-1-1\"") <
                saved.find("\"id\": \"fan-2-18\""),
            "saved dashboard was not canonicalized");

    const auto* config = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::DashboardConfigTopic);
    Require(config != nullptr && config->retained &&
                config->payload.find("\"revision\": 1") != std::string::npos,
            "saved dashboard was not republished retained");
    const auto* operation = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::DashboardConfigResultTopic);
    Require(operation != nullptr && !operation->retained,
            "dashboard operation result must not be retained");
    Require(operation->payload.find("\"success\":true") != std::string::npos &&
                operation->payload.find("\"referenceIssues\":1") != std::string::npos,
            "dashboard result does not report stale reference warning");
}

void TestDashboardRevisionConflictPreservesFile() {
    Fixture fixture;
    WriteFile(fixture.dashboard, R"json({
      "version":1,"revision":7,"title":"Current",
      "background":{"file":"","naturalWidth":0,"naturalHeight":0,
                    "defaultScale":1,"fit":"contain"},
      "fans":[]
    })json");
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    const std::string original = ReadFile(fixture.dashboard);
    fixture.client.publications.clear();

    fixture.client.Inject(
        mdvwb::ManagerMqttService::DashboardConfigSetTopic,
        R"json({
          "version":1,"revision":6,"title":"Stale editor",
          "background":{"file":"","naturalWidth":0,"naturalHeight":0,
                        "defaultScale":1,"fit":"contain"},
          "fans":[]
        })json");
    const auto result = service.ProcessOne();
    Require(result.has_value() && !result->success && !result->saved,
            "stale dashboard revision was accepted");
    Require(ReadFile(fixture.dashboard) == original,
            "revision conflict changed dashboard file");
    const auto* operation = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::DashboardConfigResultTopic);
    Require(operation != nullptr &&
                operation->payload.find("revision conflict") != std::string::npos &&
                operation->payload.find("\"revision\":7") != std::string::npos,
            "revision conflict result is incomplete");
}

void TestInvalidAndRetainedDashboardCommandsAreRejected() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    const std::string original = ReadFile(fixture.dashboard);

    fixture.client.Inject(
        mdvwb::ManagerMqttService::DashboardConfigSetTopic,
        R"json({"version":1,"revision":0,"title":"Broken"})json");
    auto result = service.ProcessOne();
    Require(result.has_value() && !result->success && !result->saved,
            "invalid dashboard configuration was accepted");
    Require(ReadFile(fixture.dashboard) == original,
            "invalid dashboard configuration changed file");

    fixture.client.Inject(
        mdvwb::ManagerMqttService::DashboardConfigSetTopic,
        R"json({
          "version":1,"revision":0,"title":"Retained",
          "background":{"file":"","naturalWidth":0,"naturalHeight":0,
                        "defaultScale":1,"fit":"contain"},"fans":[]
        })json",
        true);
    result = service.ProcessOne();
    Require(result.has_value() && !result->success && !result->saved,
            "retained dashboard configuration was accepted");
    Require(ReadFile(fixture.dashboard) == original,
            "retained dashboard command changed file");
}

void TestBackgroundUploadUpdatesDashboardWithoutServiceRestart() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    fixture.runner.commands.clear();
    fixture.client.publications.clear();

    const std::string image = PngHeader(2048, 1024);
    const std::string uploadId = "floor_plan_1";
    const std::string sha256 = mdvwb::ComputeSha256Hex(image);
    const std::string startPayload =
        "{\"version\":1,\"uploadId\":\"" + uploadId +
        "\",\"fileName\":\"floor.png\",\"size\":" +
        std::to_string(image.size()) + ",\"sha256\":\"" + sha256 +
        "\",\"revision\":0}";

    fixture.client.Inject(
        mdvwb::ManagerMqttService::BackgroundUploadStartTopic, startPayload);
    auto result = service.ProcessOne();
    Require(result.has_value() && result->success && !result->saved &&
                result->command == "background-upload-start",
            "background upload did not start");

    fixture.client.Inject(
        "/mdvwb/dashboard/background/upload/chunk/" + uploadId + "/0",
        std::string(std::string_view(image).substr(0, 12)));
    result = service.ProcessOne();
    Require(result.has_value() && result->success,
            "first background upload chunk failed");

    fixture.client.Inject(
        "/mdvwb/dashboard/background/upload/chunk/" + uploadId + "/1",
        std::string(std::string_view(image).substr(12)));
    result = service.ProcessOne();
    Require(result.has_value() && result->success,
            "second background upload chunk failed");

    fixture.client.Inject(
        "/mdvwb/dashboard/background/upload/finish/" + uploadId, "");
    result = service.ProcessOne();
    Require(result.has_value() && result->success && result->saved &&
                result->command == "background-upload-finish",
            "background upload did not finish");
    Require(fixture.runner.commands.empty(),
            "background upload unexpectedly invoked systemd");

    const std::string saved = ReadFile(fixture.dashboard);
    Require(saved.find("\"revision\": 1") != std::string::npos,
            "background upload did not increment dashboard revision");
    Require(saved.find("\"naturalWidth\": 2048") != std::string::npos &&
                saved.find("\"naturalHeight\": 1024") != std::string::npos,
            "background dimensions were not saved");
    const std::string finalName = "background-" + sha256.substr(0, 16) + ".png";
    Require(saved.find(finalName) != std::string::npos,
            "content-addressed background file was not saved in dashboard config");
    Require(std::filesystem::exists(
                fixture.dashboard.parent_path() / "assets" / finalName),
            "uploaded background asset is missing");

    const auto* uploadResult = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::BackgroundUploadResultTopic);
    Require(uploadResult != nullptr && !uploadResult->retained &&
                uploadResult->payload.find("\"success\":true") != std::string::npos &&
                uploadResult->payload.find("\"width\":2048") != std::string::npos &&
                uploadResult->payload.find("\"revision\":1") != std::string::npos,
            "background upload result is incomplete");
    const auto* config = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::DashboardConfigTopic);
    Require(config != nullptr && config->retained &&
                config->payload.find(finalName) != std::string::npos,
            "updated dashboard was not republished retained");
}


void TestMultiplePanelsAndPanelSpecificBackground() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    fixture.client.publications.clear();

    fixture.client.Inject(
        mdvwb::ManagerMqttService::DashboardConfigSetTopic,
        R"json({
          "version":2,
          "revision":0,
          "defaultPanel":"main",
          "panels":[
            {
              "id":"main","title":"Главная",
              "background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
              "fans":[{"id":"fan-1-1","number":1,"bus":1,"address":1,"label":"Приёмная","x":0.25,"y":0.5,"markerScale":1,"rotation":0,"visible":true}]
            },
            {
              "id":"floor-2","title":"Второй этаж",
              "background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
              "fans":[{"id":"fan-1-3","number":101,"bus":1,"address":3,"label":"Кабинет 201","x":0.75,"y":0.5,"markerScale":1,"rotation":0,"visible":true}]
            }
          ]
        })json");
    auto result = service.ProcessOne();
    Require(result.has_value() && result->success && result->saved,
            "multiple dashboard panels were not saved");

    auto collection = mdvwb::LoadDashboardCollection(fixture.dashboard);
    Require(collection.version == 2 && collection.panels.size() == 2U,
            "saved dashboard collection does not contain two panels");
    Require(mdvwb::FindDashboardPanel(collection, "floor-2") != nullptr,
            "saved floor-2 panel is missing");

    const std::string image = PngHeader(1200, 800);
    const std::string uploadId = "floor2_image";
    const std::string sha256 = mdvwb::ComputeSha256Hex(image);
    const std::string startPayload =
        "{\"version\":1,\"uploadId\":\"" + uploadId +
        "\",\"fileName\":\"floor2.png\",\"panelId\":\"floor-2\",\"size\":" +
        std::to_string(image.size()) + ",\"sha256\":\"" + sha256 +
        "\",\"revision\":1}";
    fixture.client.Inject(
        mdvwb::ManagerMqttService::BackgroundUploadStartTopic, startPayload);
    result = service.ProcessOne();
    Require(result.has_value() && result->success,
            "panel-specific background upload did not start");
    fixture.client.Inject(
        "/mdvwb/dashboard/background/upload/chunk/" + uploadId + "/0", image);
    result = service.ProcessOne();
    Require(result.has_value() && result->success,
            "panel-specific background chunk failed");
    fixture.client.Inject(
        "/mdvwb/dashboard/background/upload/finish/" + uploadId, "");
    result = service.ProcessOne();
    Require(result.has_value() && result->success && result->saved,
            "panel-specific background upload did not finish");

    collection = mdvwb::LoadDashboardCollection(fixture.dashboard);
    const auto* mainPanel = mdvwb::FindDashboardPanel(collection, "main");
    const auto* floorPanel = mdvwb::FindDashboardPanel(collection, "floor-2");
    Require(mainPanel != nullptr && mainPanel->background.file.empty(),
            "upload for floor-2 changed main panel background");
    Require(floorPanel != nullptr && !floorPanel->background.file.empty() &&
                floorPanel->background.naturalWidth == 1200 &&
                floorPanel->background.naturalHeight == 800,
            "floor-2 background was not updated independently");

    fixture.client.Inject(
        mdvwb::ManagerMqttService::BackgroundUploadStartTopic,
        "{\"version\":1,\"uploadId\":\"missing_panel\",\"fileName\":\"x.png\",\"panelId\":\"missing\",\"size\":" +
        std::to_string(image.size()) + ",\"sha256\":\"" + sha256 +
        "\",\"revision\":2}");
    result = service.ProcessOne();
    Require(result.has_value() && !result->success,
            "upload for unknown panel was accepted");
}

void WriteDashboardWithMainFan(const Fixture& fixture) {
    WriteFile(fixture.dashboard, R"json({
      "version":2,"revision":0,"defaultPanel":"main","panels":[{
        "id":"main","title":"Главная",
        "background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
        "fans":[{"id":"fan-1-1","number":1,"bus":1,"address":1,"label":"Кабинет 1",
                 "x":0.5,"y":0.5,"markerScale":1,"rotation":0,"visible":true}]
      }]
    })json");
}

std::string ValidSchedulesPayload(int revision = 0) {
    return std::string("{\"version\":1,\"revision\":") + std::to_string(revision) + R"json(,
      "schedules":[{
        "id":"morning","name":"Утренний запуск","enabled":true,"panelId":"main",
        "kind":"weekly","days":[1,2,3,4,5],"date":"","time":"08:00",
        "targets":[{"bus":1,"address":1}],
        "actions":{"power":true,"mode":0,"speed":2,"setTemp":23}
      }]})json";
}

void TestSchedulesConfigurationIsSavedAndRunIsQueued() {
    Fixture fixture;
    WriteDashboardWithMainFan(fixture);
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    fixture.client.publications.clear();

    fixture.client.Inject(
        mdvwb::ManagerMqttService::SchedulesConfigSetTopic,
        ValidSchedulesPayload());
    auto result = service.ProcessOne();
    Require(result.has_value() && result->success && result->saved &&
                result->command == "schedules-config",
            "valid schedules configuration was not saved");
    const std::string saved = ReadFile(fixture.schedules);
    Require(saved.find("\"revision\": 1") != std::string::npos &&
                saved.find("\"id\": \"morning\"") != std::string::npos,
            "saved schedules file is incomplete");

    const auto* config = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::SchedulesConfigTopic);
    Require(config != nullptr && config->retained &&
                config->payload.find("\"revision\": 1") != std::string::npos,
            "saved schedules were not republished retained");
    const auto* operation = LastPublication(
        fixture.client, mdvwb::ManagerMqttService::SchedulesConfigResultTopic);
    Require(operation != nullptr && !operation->retained &&
                operation->payload.find("\"success\":true") != std::string::npos &&
                operation->payload.find("\"schedules\":1") != std::string::npos,
            "schedules operation result is incomplete");

    fixture.client.Inject("/mdvwb/schedules/morning/run", "1");
    result = service.ProcessOne();
    Require(result.has_value() && result->success && !result->saved &&
                result->command == "schedule-run",
            "manual schedule run was not queued");
    const auto* execute = LastPublication(
        fixture.client, "/mdvwb/schedules/morning/execute");
    Require(execute != nullptr && !execute->retained &&
                execute->payload.find("\"source\":\"manual\"") != std::string::npos,
            "manual schedule execution event is missing");
    const auto* runResult = LastPublication(
        fixture.client, "/mdvwb/schedules/morning/result");
    Require(runResult != nullptr && !runResult->retained &&
                runResult->payload.find("\"state\":\"queued\"") != std::string::npos,
            "queued schedule result is missing");
}

void TestSchedulesConflictsReferencesAndRetainedRunsAreRejected() {
    Fixture fixture;
    WriteDashboardWithMainFan(fixture);
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();

    fixture.client.Inject(
        mdvwb::ManagerMqttService::SchedulesConfigSetTopic,
        ValidSchedulesPayload());
    auto result = service.ProcessOne();
    Require(result.has_value() && result->success,
            "initial schedules save failed");
    const std::string original = ReadFile(fixture.schedules);

    fixture.client.Inject(
        mdvwb::ManagerMqttService::SchedulesConfigSetTopic,
        ValidSchedulesPayload(0));
    result = service.ProcessOne();
    Require(result.has_value() && !result->success && !result->saved &&
                ReadFile(fixture.schedules) == original,
            "stale schedules revision changed the file");

    fixture.client.Inject(
        mdvwb::ManagerMqttService::SchedulesConfigSetTopic,
        R"json({"version":1,"revision":1,"schedules":[{
          "id":"bad-target","name":"Bad","enabled":true,"panelId":"main",
          "kind":"weekly","days":[1],"date":"","time":"08:00",
          "targets":[{"bus":1,"address":63}],"actions":{"power":true}
        }]})json");
    result = service.ProcessOne();
    Require(result.has_value() && !result->success && !result->saved &&
                ReadFile(fixture.schedules) == original,
            "schedule with unknown address was accepted");

    fixture.client.Inject("/mdvwb/schedules/morning/run", "1", true);
    result = service.ProcessOne();
    Require(result.has_value() && !result->success,
            "retained schedule run was accepted");

    fixture.client.Inject("/mdvwb/schedules/missing/run", "1");
    result = service.ProcessOne();
    Require(result.has_value() && !result->success,
            "unknown schedule run was accepted");
}

void TestInvalidConfigurationAndSynchronizationFailure() {
    Fixture fixture;
    mdvwb::ManagerMqttService service(
        fixture.client, fixture.config, fixture.paths, fixture.runner, &fixture.discovery);
    service.Start();
    const std::string original = ReadFile(fixture.config);

    fixture.client.Inject(
        mdvwb::ManagerMqttService::ConfigSetTopic,
        R"json({"version":1,"buses":[
          {"id":1,"enabled":true,"port":"tty0","addresses":[1]}
        ]})json");
    const auto invalid = service.ProcessOne();
    Require(invalid.has_value() && !invalid->success && !invalid->saved,
            "invalid configuration was accepted");
    Require(ReadFile(fixture.config) == original,
            "invalid configuration changed the file");

    fixture.runner.returnCode = 5;
    fixture.client.Inject(
        mdvwb::ManagerMqttService::ConfigSetTopic,
        R"json({"version":1,"buses":[
          {"id":1,"enabled":true,"port":"/dev/ttyRS485-9","addresses":[9]}
        ]})json");
    const auto failed = service.ProcessOne();
    Require(failed.has_value() && !failed->success && failed->saved,
            "synchronization failure must report saved=true");
    Require(ReadFile(fixture.config).find("/dev/ttyRS485-9") != std::string::npos,
            "validated configuration was not saved before synchronization");
}

}  // namespace

int main() {
    try {
        TestStartPublishesConfigurationAndBusStatuses();
        TestValidConfigurationIsSavedAndApplied();
        TestBusStartStopRestartAndStatus();
        TestUnknownDisabledAndRetainedBusCommandsAreRejected();
        TestDiscoveryStopsOnlySelectedBusAndPublishesResult();
        TestDiscoveryRejectsUnknownRetainedAndFailedRuns();
        TestRemovedDevicesClearRetainedTopics();
        TestDashboardConfigurationIsSavedWithRevisionAndWarnings();
        TestDashboardRevisionConflictPreservesFile();
        TestInvalidAndRetainedDashboardCommandsAreRejected();
        TestBackgroundUploadUpdatesDashboardWithoutServiceRestart();
        TestMultiplePanelsAndPanelSpecificBackground();
        TestSchedulesConfigurationIsSavedAndRunIsQueued();
        TestSchedulesConflictsReferencesAndRetainedRunsAreRejected();
        TestInvalidConfigurationAndSynchronizationFailure();
        std::cout << "MDVWB manager MQTT and dashboard tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB manager MQTT and dashboard tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
