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

    Require(fixture.client.subscriptions.size() == 6,
            "manager must create six subscriptions");
    Require(fixture.client.subscriptions[0] == mdvwb::ManagerMqttService::ConfigSetTopic,
            "manager subscribed to wrong configuration topic");
    Require(fixture.client.subscriptions[1] == mdvwb::ManagerMqttService::BusStartFilter,
            "manager subscribed to wrong start filter");
    Require(fixture.client.subscriptions[2] == mdvwb::ManagerMqttService::BusStopFilter,
            "manager subscribed to wrong stop filter");
    Require(fixture.client.subscriptions[3] == mdvwb::ManagerMqttService::BusRestartFilter,
            "manager subscribed to wrong restart filter");
    Require(fixture.client.subscriptions[4] == mdvwb::ManagerMqttService::BusStatusGetFilter,
            "manager subscribed to wrong status filter");
    Require(fixture.client.subscriptions[5] == mdvwb::ManagerMqttService::BusDiscoveryStartFilter,
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
        TestInvalidConfigurationAndSynchronizationFailure();
        std::cout << "MDVWB manager MQTT discovery tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB manager MQTT discovery tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
