#include "mdvwb_scheduler.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using mdv::MqttMessage;
using mdv::MqttPublication;
using mdvwb::SchedulerClock;
using mdvwb::SchedulerLocalMinute;
using mdvwb::SchedulerPaths;
using mdvwb::SchedulerService;

class FakeClock final : public SchedulerClock {
public:
    SchedulerLocalMinute minute{
        2026,
        7,
        20,
        8,
        0,
        1};
    std::chrono::steady_clock::time_point monotonic{};

    [[nodiscard]] SchedulerLocalMinute LocalMinute() const override
    {
        return minute;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point MonotonicNow() const override
    {
        return monotonic;
    }

    void AdvanceSeconds(int seconds)
    {
        monotonic += std::chrono::seconds(seconds);
    }
};

class FakeMqttClient final : public mdv::IMqttClient {
public:
    void SetMessageHandler(MessageHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void Subscribe(std::string_view filter) override
    {
        subscriptions.emplace_back(filter);
    }

    void Publish(
        std::string_view topic,
        std::string_view payload,
        bool retained) override
    {
        publications.push_back(MqttPublication{
            std::string(topic),
            std::string(payload),
            retained});
    }

    void Inject(std::string topic, std::string payload, bool retained = false)
    {
        if (!handler_) {
            throw std::runtime_error("message handler is not installed");
        }
        handler_(MqttMessage{
            std::move(topic),
            std::move(payload),
            retained});
    }

    [[nodiscard]] std::size_t CountTopic(std::string_view topic) const
    {
        std::size_t count = 0;
        for (const MqttPublication& publication : publications) {
            if (publication.topic == topic) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] const MqttPublication* LastTopic(std::string_view topic) const
    {
        for (auto it = publications.rbegin(); it != publications.rend(); ++it) {
            if (it->topic == topic) {
                return &*it;
            }
        }
        return nullptr;
    }

    MessageHandler handler_;
    std::vector<std::string> subscriptions;
    std::vector<MqttPublication> publications;
};

struct TestEnvironment {
    std::filesystem::path root;
    SchedulerPaths paths;

    TestEnvironment()
    {
        root = std::filesystem::temp_directory_path() /
            ("mdvwb-scheduler-test-" + std::to_string(counter++));
        std::filesystem::create_directories(root);
        paths.schedules = root / "schedules.json";
        paths.buses = root / "buses.json";
        paths.dashboard = root / "dashboard.json";
        paths.state = root / "scheduler-state.tsv";
        paths.confirmationTimeoutSeconds = 10;
        WriteFiles();
    }

    ~TestEnvironment()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

private:
    void WriteFiles() const
    {
        Write(
            paths.buses,
            R"json({
              "version":1,
              "buses":[{"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1,2]}]
            })json");
        Write(
            paths.dashboard,
            R"json({
              "version":2,
              "revision":1,
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
        Write(
            paths.schedules,
            R"json({
              "version":1,
              "revision":1,
              "schedules":[
                {
                  "id":"workday-start","name":"Workday start","panelId":"main","enabled":true,
                  "kind":"weekly","days":[1],"date":"","time":"08:00",
                  "targets":[{"bus":1,"address":1}],
                  "actions":{"power":true,"mode":0,"speed":2,"setTemp":23}
                },
                {
                  "id":"manual-off","name":"Manual off","panelId":"main","enabled":false,
                  "kind":"once","days":[],"date":"2026-07-21","time":"18:00",
                  "targets":[{"bus":1,"address":2}],"actions":{"power":false}
                }
              ]
            })json");
    }

    static void Write(
        const std::filesystem::path& path,
        std::string_view contents)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
        if (!output) {
            throw std::runtime_error("cannot write test fixture");
        }
    }

    inline static int counter = 0;
};

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void Drain(SchedulerService& service)
{
    while (service.ProcessOne()) {
    }
}

std::string FactTopic(int address, std::string_view control)
{
    return "/devices/Fan-1_" + std::to_string(address) +
        "/controls/" + std::string(control);
}

std::string CommandTopic(int address, std::string_view control)
{
    return FactTopic(address, control) + "/on1";
}

void InjectDesiredWorkdayFacts(
    FakeMqttClient& mqtt,
    bool retained)
{
    mqtt.Inject(FactTopic(1, "Mode"), "0", retained);
    mqtt.Inject(FactTopic(1, "Speed"), "2", retained);
    mqtt.Inject(FactTopic(1, "SetTemp"), "23", retained);
    mqtt.Inject(FactTopic(1, "Power"), "1", retained);
}

void TestSentCommandsRequireFreshLiveFacts()
{
    TestEnvironment environment;
    FakeClock clock;
    FakeMqttClient mqtt;
    SchedulerService service(mqtt, environment.paths, clock);
    service.Start();

    // These values match the schedule but no online Status is known. They must
    // not suppress commands or confirm them after the run starts.
    InjectDesiredWorkdayFacts(mqtt, true);
    Drain(service);

    service.Tick();
    Require(service.HasActiveRun(), "automatic run should wait for confirmation");
    Require(mqtt.CountTopic(CommandTopic(1, "Mode")) == 1U,
        "Mode command should be published");
    Require(mqtt.CountTopic(CommandTopic(1, "Speed")) == 1U,
        "Speed command should be published");
    Require(mqtt.CountTopic(CommandTopic(1, "SetTemp")) == 1U,
        "SetTemp command should be published");
    Require(mqtt.CountTopic(CommandTopic(1, "Power")) == 1U,
        "Power command should be published");

    // A retained replay received after publication is still not a live
    // confirmation of the physical command.
    InjectDesiredWorkdayFacts(mqtt, true);
    Drain(service);
    Require(service.HasActiveRun(),
        "retained factual replay must not confirm sent commands");

    // Fresh non-retained factual updates after the commands confirm the run.
    InjectDesiredWorkdayFacts(mqtt, false);
    Drain(service);
    Require(!service.HasActiveRun(),
        "fresh live facts should complete the run");

    const MqttPublication* result = mqtt.LastTopic(
        "/mdvwb/schedules/workday-start/result");
    Require(result != nullptr, "completed result should be published");
    Require(result->payload.find("\"state\":\"completed\"") != std::string::npos,
        "result should report completed state");
}

void TestOnlineAlreadySatisfiedSkipsDuplicateCommand()
{
    TestEnvironment environment;
    FakeClock clock;
    FakeMqttClient mqtt;
    SchedulerService service(mqtt, environment.paths, clock);
    service.Start();

    mqtt.Inject(FactTopic(2, "Status"), "0", true);
    mqtt.Inject(FactTopic(2, "Power"), "0", true);
    Drain(service);

    mqtt.Inject("/mdvwb/schedules/manual-off/execute", "1", false);
    Drain(service);

    Require(!service.HasActiveRun(),
        "already satisfied online target should complete immediately");
    Require(mqtt.CountTopic(CommandTopic(2, "Power")) == 0U,
        "duplicate Power command should not be published");

    const MqttPublication* result = mqtt.LastTopic(
        "/mdvwb/schedules/manual-off/result");
    Require(result != nullptr, "manual result should be published");
    Require(result->payload.find("\"state\":\"completed\"") != std::string::npos,
        "already satisfied run should complete");
    Require(result->payload.find("\"commands\":0") != std::string::npos,
        "already satisfied run should report zero commands");
}

void TestOfflineTargetFailsEvenWhenOldValueMatches()
{
    TestEnvironment environment;
    FakeClock clock;
    FakeMqttClient mqtt;
    SchedulerService service(mqtt, environment.paths, clock);
    service.Start();

    mqtt.Inject(FactTopic(2, "Power"), "0", true);
    mqtt.Inject(FactTopic(2, "Status"), "7", true);
    Drain(service);

    mqtt.Inject("/mdvwb/schedules/manual-off/execute", "1", false);
    Drain(service);

    Require(!service.HasActiveRun(), "offline target should not remain active");
    Require(mqtt.CountTopic(CommandTopic(2, "Power")) == 0U,
        "offline target must not receive a command");

    const MqttPublication* result = mqtt.LastTopic(
        "/mdvwb/schedules/manual-off/result");
    Require(result != nullptr, "offline result should be published");
    Require(result->payload.find("\"state\":\"failed\"") != std::string::npos,
        "offline run should fail");
    Require(result->payload.find("offline") != std::string::npos,
        "offline failure should explain the reason");
}

void TestTargetGoingOfflineFailsActiveRun()
{
    TestEnvironment environment;
    FakeClock clock;
    FakeMqttClient mqtt;
    SchedulerService service(mqtt, environment.paths, clock);
    service.Start();

    service.Tick();
    Require(service.HasActiveRun(), "automatic run should be active");

    mqtt.Inject(FactTopic(1, "Status"), "7", false);
    Drain(service);

    Require(!service.HasActiveRun(),
        "active run should fail when target reports offline");
    const MqttPublication* result = mqtt.LastTopic(
        "/mdvwb/schedules/workday-start/result");
    Require(result != nullptr, "offline transition result should be published");
    Require(result->payload.find("\"state\":\"failed\"") != std::string::npos,
        "offline transition should fail the run");
}

void TestAutomaticRunIsNotRepeatedAfterRestart()
{
    TestEnvironment environment;
    FakeClock firstClock;
    FakeMqttClient firstMqtt;
    {
        SchedulerService service(firstMqtt, environment.paths, firstClock);
        service.Start();
        service.Tick();
        Require(service.HasActiveRun(), "first automatic run should start");
    }

    FakeClock secondClock;
    FakeMqttClient secondMqtt;
    SchedulerService restarted(secondMqtt, environment.paths, secondClock);
    restarted.Start();
    restarted.Tick();

    Require(!restarted.HasActiveRun(),
        "restart in same minute must not repeat automatic run");
    Require(secondMqtt.CountTopic(CommandTopic(1, "Power")) == 0U,
        "restart should not publish duplicate command");
}

void TestConfirmationTimeout()
{
    TestEnvironment environment;
    FakeClock clock;
    FakeMqttClient mqtt;
    SchedulerService service(mqtt, environment.paths, clock);
    service.Start();

    service.Tick();
    Require(service.HasActiveRun(), "automatic run should start");

    clock.AdvanceSeconds(11);
    service.Tick();
    Require(!service.HasActiveRun(), "run should stop after timeout");

    const MqttPublication* result = mqtt.LastTopic(
        "/mdvwb/schedules/workday-start/result");
    Require(result != nullptr, "timeout result should be published");
    Require(result->payload.find("\"state\":\"timeout\"") != std::string::npos,
        "result should report timeout");
}

} // namespace

int main()
{
    try {
        TestSentCommandsRequireFreshLiveFacts();
        TestOnlineAlreadySatisfiedSkipsDuplicateCommand();
        TestOfflineTargetFailsEvenWhenOldValueMatches();
        TestTargetGoingOfflineFailsActiveRun();
        TestAutomaticRunIsNotRepeatedAfterRestart();
        TestConfirmationTimeout();
        std::cout << "mdvwb_scheduler_test: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "mdvwb_scheduler_test: " << error.what() << '\n';
        return 1;
    }
}
