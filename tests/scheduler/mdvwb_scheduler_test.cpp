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
            ("mdvwb-scheduler-test-" + std::to_string(token));
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
    void SetMessageHandler(MessageHandler handler) override { handler_ = std::move(handler); }
    void Subscribe(std::string_view topicFilter) override {
        subscriptions.emplace_back(topicFilter);
    }
    void Publish(std::string_view topic, std::string_view payload, bool retained) override {
        publications.push_back({std::string(topic), std::string(payload), retained});
    }
    void Inject(std::string topic, std::string payload, bool retained = false) {
        Require(static_cast<bool>(handler_), "handler must be installed");
        handler_({std::move(topic), std::move(payload), retained});
    }

    MessageHandler handler_;
    std::vector<std::string> subscriptions;
    std::vector<mdv::MqttPublication> publications;
};

class FakeClock final : public mdvwb::SchedulerClock {
public:
    mdvwb::SchedulerLocalMinute LocalMinute() const override { return minute; }
    std::chrono::steady_clock::time_point MonotonicNow() const override { return monotonic; }

    mdvwb::SchedulerLocalMinute minute{2026, 7, 27, 8, 0, 1};
    std::chrono::steady_clock::time_point monotonic{};
};

void WriteFile(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write test file");
    }
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

std::size_t CountTopic(const FakeMqttClient& client, std::string_view topic) {
    std::size_t count = 0;
    for (const auto& publication : client.publications) {
        if (publication.topic == topic) {
            ++count;
        }
    }
    return count;
}

struct Fixture {
    TemporaryDirectory temporary;
    mdvwb::SchedulerPaths paths;
    FakeMqttClient client;
    FakeClock clock;

    Fixture() {
        paths.buses = temporary.Path() / "etc/mdvwb/buses.json";
        paths.dashboard = temporary.Path() / "etc/mdvwb/dashboard.json";
        paths.schedules = temporary.Path() / "etc/mdvwb/schedules.json";
        paths.state = temporary.Path() / "var/lib/mdvwb/scheduler-state.tsv";
        paths.confirmationTimeoutSeconds = 10;

        WriteFile(paths.buses, R"json({
          "version":1,
          "buses":[{"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1,2]}]
        })json");
        WriteFile(paths.dashboard, R"json({
          "version":2,
          "revision":0,
          "defaultPanel":"main",
          "panels":[{
            "id":"main","title":"Main",
            "background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
            "fans":[
              {"id":"fan-1-1","number":1,"bus":1,"address":1,"label":"A","x":0.2,"y":0.3,"markerScale":1,"rotation":0,"visible":true},
              {"id":"fan-1-2","number":2,"bus":1,"address":2,"label":"B","x":0.4,"y":0.5,"markerScale":1,"rotation":0,"visible":true}
            ]
          }]
        })json");
        WriteFile(paths.schedules, R"json({
          "version":1,
          "revision":3,
          "schedules":[
            {
              "id":"workday-start","name":"Start","enabled":true,"panelId":"main",
              "kind":"weekly","days":[1,2,3,4,5],"date":"","time":"08:00",
              "targets":[{"bus":1,"address":1}],
              "actions":{"power":true,"mode":0,"speed":2,"setTemp":23}
            },
            {
              "id":"manual-off","name":"Manual","enabled":false,"panelId":"main",
              "kind":"once","days":[],"date":"2026-12-31","time":"18:00",
              "targets":[{"bus":1,"address":2}],"actions":{"power":false}
            }
          ]
        })json");
    }
};

void Drain(mdvwb::SchedulerService& service) {
    while (service.ProcessOne().has_value()) {
    }
}

void TestAutomaticExecutionAndConfirmation() {
    Fixture fixture;
    mdvwb::SchedulerService service(fixture.client, fixture.paths, fixture.clock);
    service.Start();

    Require(fixture.client.subscriptions.size() == 3U, "scheduler must subscribe to three filters");
    Require(fixture.client.subscriptions[0] == mdvwb::SchedulerService::ConfigTopic,
            "wrong config subscription");
    Require(fixture.client.subscriptions[1] == mdvwb::SchedulerService::ExecuteFilter,
            "wrong execute subscription");
    Require(fixture.client.subscriptions[2] == mdvwb::SchedulerService::FactsFilter,
            "wrong fact subscription");

    service.Tick();
    Require(service.HasActiveRun(), "due schedule must become active");
    const std::vector<std::string> expectedTopics = {
        "/devices/Fan-1_1/controls/Mode/on1",
        "/devices/Fan-1_1/controls/Speed/on1",
        "/devices/Fan-1_1/controls/SetTemp/on1",
        "/devices/Fan-1_1/controls/Power/on1",
    };
    for (const auto& topic : expectedTopics) {
        Require(CountTopic(fixture.client, topic) == 1U, "missing scheduled control command");
    }

    fixture.client.Inject("/devices/Fan-1_1/controls/Mode", "0", true);
    fixture.client.Inject("/devices/Fan-1_1/controls/Speed", "2", true);
    fixture.client.Inject("/devices/Fan-1_1/controls/SetTemp", "23", true);
    fixture.client.Inject("/devices/Fan-1_1/controls/Power", "1", true);
    Drain(service);
    Require(!service.HasActiveRun(), "run must finish after all facts are confirmed");

    const auto* result = LastPublication(
        fixture.client, "/mdvwb/schedules/workday-start/result");
    Require(result != nullptr && result->payload.find("\"state\":\"completed\"") != std::string::npos,
            "completed result was not published");

    service.Tick();
    Require(CountTopic(fixture.client, expectedTopics[0]) == 1U,
            "automatic schedule must execute only once in the same minute");
    Require(std::filesystem::exists(fixture.paths.state), "automatic execution state must be persisted");
}

void TestRestartDoesNotRepeatSameMinute() {
    Fixture fixture;
    {
        mdvwb::SchedulerService first(fixture.client, fixture.paths, fixture.clock);
        first.Start();
        first.Tick();
    }

    FakeMqttClient secondClient;
    mdvwb::SchedulerService second(secondClient, fixture.paths, fixture.clock);
    second.Start();
    second.Tick();
    Require(CountTopic(secondClient, "/devices/Fan-1_1/controls/Mode/on1") == 0U,
            "restart in the same minute must not repeat automatic execution");
}

void TestDisabledScheduleCanRunManually() {
    Fixture fixture;
    mdvwb::SchedulerService service(fixture.client, fixture.paths, fixture.clock);
    service.Start();
    fixture.client.Inject(
        "/mdvwb/schedules/manual-off/execute",
        "{\"version\":1,\"scheduleId\":\"manual-off\",\"source\":\"manual\"}");
    Drain(service);
    Require(CountTopic(fixture.client, "/devices/Fan-1_2/controls/Power/on1") == 1U,
            "disabled schedule must support manual execution");

    fixture.client.Inject("/devices/Fan-1_2/controls/Power", "0", true);
    Drain(service);
    const auto* result = LastPublication(fixture.client, "/mdvwb/schedules/manual-off/result");
    Require(result != nullptr && result->payload.find("\"state\":\"completed\"") != std::string::npos,
            "manual run must publish completed result");
}

void TestConfirmationTimeout() {
    Fixture fixture;
    mdvwb::SchedulerService service(fixture.client, fixture.paths, fixture.clock);
    service.Start();
    service.Tick();
    fixture.clock.monotonic += std::chrono::seconds(11);
    service.Tick();
    const auto* result = LastPublication(
        fixture.client, "/mdvwb/schedules/workday-start/result");
    Require(result != nullptr && result->payload.find("\"state\":\"timeout\"") != std::string::npos,
            "unconfirmed run must time out");
}

} // namespace

int main() {
    try {
        TestAutomaticExecutionAndConfirmation();
        TestRestartDoesNotRepeatSameMinute();
        TestDisabledScheduleCanRunManually();
        TestConfirmationTimeout();
        std::cout << "MDVWB scheduler tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB scheduler tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
