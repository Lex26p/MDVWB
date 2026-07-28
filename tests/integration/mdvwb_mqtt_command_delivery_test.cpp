#include "mdvwb_manager_mqtt.h"
#include "mdvwb_scheduler.h"

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

void WriteFile(const std::filesystem::path& path, std::string_view content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write MQTT delivery test fixture");
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        const auto token =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-mqtt-command-delivery-" + std::to_string(token));
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

class DeliveryMqttClient final : public mdv::IMqttClient {
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
        publications.push_back({std::string(topic), std::string(payload), retained});
    }

    [[nodiscard]] mdv::MqttPublishStatus PublishWithResult(
        std::string_view topic,
        std::string_view payload,
        bool retained) override
    {
        if (!failedTopic.empty() && topic == failedTopic) {
            ++failedAttempts;
            return failedStatus;
        }
        Publish(topic, payload, retained);
        return mdv::MqttPublishStatus::Published;
    }

    void Inject(std::string topic, std::string payload, bool retained = false)
    {
        Require(static_cast<bool>(handler_), "MQTT handler is not installed");
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

    [[nodiscard]] bool HasResultState(
        std::string_view topic,
        std::string_view state) const
    {
        const std::string marker =
            "\"state\":\"" + std::string(state) + "\"";
        for (const auto& publication : publications) {
            if (publication.topic == topic &&
                publication.payload.find(marker) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    MessageHandler handler_;
    std::vector<std::string> subscriptions;
    std::vector<mdv::MqttPublication> publications;
    std::string failedTopic;
    mdv::MqttPublishStatus failedStatus = mdv::MqttPublishStatus::Disconnected;
    int failedAttempts = 0;
};

class RecordingRunner final : public mdvwb::CommandRunner {
public:
    int Run(const std::vector<std::string>& arguments) override
    {
        if (arguments.size() >= 2U && arguments[1] == "is-active") {
            return 3;
        }
        if (arguments.size() >= 2U && arguments[1] == "is-enabled") {
            return 1;
        }
        return 0;
    }
};

class FakeClock final : public mdvwb::SchedulerClock {
public:
    [[nodiscard]] mdvwb::SchedulerLocalMinute LocalMinute() const override
    {
        return {2026, 7, 28, 12, 0, 2};
    }

    [[nodiscard]] std::int64_t UnixTimeSeconds() const override
    {
        return 1785232800;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point MonotonicNow() const override
    {
        return {};
    }
};

struct Fixture {
    TemporaryDirectory temporary;
    std::filesystem::path buses = temporary.Path() / "etc/mdvwb/buses.json";
    std::filesystem::path dashboard = temporary.Path() / "etc/mdvwb/dashboard.json";
    std::filesystem::path schedules = temporary.Path() / "etc/mdvwb/schedules.json";
    std::filesystem::path state = temporary.Path() / "var/lib/mdvwb/scheduler-state.tsv";
    std::filesystem::path assets = temporary.Path() / "var/www/fancoils/assets";
    mdvwb::ServiceSyncPaths servicePaths;

    Fixture()
    {
        servicePaths.defaultDirectory = temporary.Path() / "etc/default";
        servicePaths.environmentTemplate = temporary.Path() / "mdvwb.env";
        servicePaths.systemctlProgram = "fake-systemctl";

        WriteFile(
            servicePaths.environmentTemplate,
            "MDVWB_ADDRESSES=\"1\"\n"
            "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
            "MDVWB_BUS=\"1\"\n"
            "MDVWB_MASTER_ID=\"0\"\n");
        WriteFile(
            buses,
            R"json({
              "version":1,
              "revision":0,
              "buses":[{"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1]}]
            })json");
        WriteFile(
            dashboard,
            R"json({
              "version":2,
              "revision":0,
              "defaultPanel":"main",
              "panels":[{
                "id":"main","title":"Main",
                "background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
                "fans":[{"id":"fan-1-1","number":1,"bus":1,"address":1,"label":"A","x":0.5,"y":0.5,"markerScale":1,"rotation":0,"visible":true}]
              }]
            })json");
        WriteFile(
            schedules,
            R"json({
              "version":1,
              "revision":1,
              "schedules":[{
                "id":"manual-off","name":"Manual off","panelId":"main","enabled":false,
                "kind":"once","days":[],"date":"2026-07-28","time":"12:00",
                "targets":[{"bus":1,"address":1}],"actions":{"power":false}
              }]
            })json");
    }
};

void TestManagerRejectsUndeliveredExecuteEvent()
{
    Fixture fixture;
    DeliveryMqttClient mqtt;
    RecordingRunner runner;
    mdvwb::ManagerMqttService service(
        mqtt,
        fixture.buses,
        fixture.servicePaths,
        runner,
        nullptr,
        fixture.dashboard,
        fixture.assets,
        fixture.schedules);
    service.Start();
    mqtt.publications.clear();
    mqtt.failedTopic = "/mdvwb/schedules/manual-off/execute";

    mqtt.Inject("/mdvwb/schedules/manual-off/run", "run", false);
    const auto result = service.ProcessOne();

    Require(result.has_value() && !result->success && !result->saved,
        "manager reported an undelivered execute event as queued");
    Require(result->message.find("disconnected") != std::string::npos,
        "manager delivery failure does not explain the disconnect");
    Require(mqtt.failedAttempts == 1,
        "manager did not attempt the scheduler execute publication exactly once");
    Require(!mqtt.HasResultState(
                "/mdvwb/schedules/manual-off/result", "queued"),
        "manager published queued after execute delivery failed");
    Require(mqtt.HasResultState(
                "/mdvwb/schedules/manual-off/result", "rejected"),
        "manager did not publish a rejected run result");
}

void TestSchedulerFailsImmediatelyWhenCommandIsUndelivered()
{
    Fixture fixture;
    DeliveryMqttClient mqtt;
    FakeClock clock;
    mdvwb::SchedulerPaths paths;
    paths.buses = fixture.buses;
    paths.dashboard = fixture.dashboard;
    paths.schedules = fixture.schedules;
    paths.state = fixture.state;
    paths.confirmationTimeoutSeconds = 10;

    mdvwb::SchedulerService service(mqtt, paths, clock);
    service.Start();
    mqtt.publications.clear();
    mqtt.failedTopic =
        "/devices/Fan-1_1/controls/Power/on1";

    mqtt.Inject("/mdvwb/schedules/manual-off/execute", "1", false);
    const auto accepted = service.ProcessOne();

    Require(accepted.has_value(), "scheduler did not process manual execution");
    Require(!service.HasActiveRun(),
        "scheduler waited for a timeout after command delivery failed");
    Require(mqtt.failedAttempts == 1,
        "scheduler did not attempt the Power command exactly once");
    Require(!mqtt.HasResultState(
                "/mdvwb/schedules/manual-off/result", "executing"),
        "scheduler published executing after command delivery failed");
    Require(mqtt.HasResultState(
                "/mdvwb/schedules/manual-off/result", "failed"),
        "scheduler did not publish a failed terminal result");

    const mdv::MqttPublication* result = mqtt.Last(
        "/mdvwb/schedules/manual-off/result");
    Require(result != nullptr &&
            result->payload.find("disconnected") != std::string::npos,
        "scheduler failure does not explain the MQTT disconnect");
}

} // namespace

int main()
{
    try {
        TestManagerRejectsUndeliveredExecuteEvent();
        TestSchedulerFailsImmediatelyWhenCommandIsUndelivered();
        std::cout << "MDVWB MQTT command delivery tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB MQTT command delivery tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
