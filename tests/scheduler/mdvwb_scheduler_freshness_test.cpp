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

class FakeClock final : public mdvwb::SchedulerClock {
public:
    [[nodiscard]] mdvwb::SchedulerLocalMinute LocalMinute() const override
    {
        return {2026, 7, 20, 7, 30, 1};
    }

    [[nodiscard]] std::int64_t UnixTimeSeconds() const override
    {
        return 1784532600;
    }

    [[nodiscard]] std::chrono::steady_clock::time_point MonotonicNow() const override
    {
        return {};
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
        publications.push_back({
            std::string(topic),
            std::string(payload),
            retained});
    }

    void Inject(std::string topic, std::string payload, bool retained = false)
    {
        Require(static_cast<bool>(handler_), "MQTT handler is not installed");
        handler_({std::move(topic), std::move(payload), retained});
    }

    [[nodiscard]] std::size_t CountTopic(std::string_view topic) const
    {
        std::size_t count = 0U;
        for (const auto& publication : publications) {
            if (publication.topic == topic) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] const mdv::MqttPublication* LastTopic(
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

class TestEnvironment final {
public:
    TestEnvironment()
    {
        const auto token =
            std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            ("mdvwb-scheduler-freshness-" + std::to_string(token));
        std::filesystem::create_directories(root);
        paths.schedules = root / "schedules.json";
        paths.buses = root / "buses.json";
        paths.dashboard = root / "dashboard.json";
        paths.state = root / "scheduler-state.tsv";
        paths.confirmationTimeoutSeconds = 10;
        WriteValidBuses();
        WriteValidDashboard();
        Write(
            paths.schedules,
            R"json({
              "version":1,
              "revision":1,
              "schedules":[{
                "id":"manual-off","name":"Manual off","panelId":"main","enabled":false,
                "kind":"once","days":[],"date":"2026-07-21","time":"18:00",
                "targets":[{"bus":1,"address":2}],"actions":{"power":false}
              }]
            })json");
    }

    ~TestEnvironment()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    void WriteValidBuses() const
    {
        Write(
            paths.buses,
            R"json({
              "version":1,
              "revision":1,
              "buses":[{"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1,2]}]
            })json");
    }

    void WriteDisabledBus() const
    {
        Write(
            paths.buses,
            R"json({
              "version":1,
              "revision":2,
              "buses":[{"id":1,"enabled":false,"port":"/dev/ttyRS485-1","addresses":[1,2]}]
            })json");
    }

    void WriteDeviceRemoved() const
    {
        Write(
            paths.buses,
            R"json({
              "version":1,
              "revision":3,
              "buses":[{"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1]}]
            })json");
    }

    void WriteValidDashboard() const
    {
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
    }

    void WriteDashboardWithoutTarget() const
    {
        Write(
            paths.dashboard,
            R"json({
              "version":2,
              "revision":2,
              "defaultPanel":"main",
              "panels":[{
                "id":"main","title":"Main",
                "background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
                "fans":[
                  {"id":"fan-1-1","number":1,"bus":1,"address":1,"label":"A","x":0.2,"y":0.3,"markerScale":1,"rotation":0,"visible":true}
                ]
              }]
            })json");
    }


    std::filesystem::path root;
    mdvwb::SchedulerPaths paths;

private:
    static void Write(
        const std::filesystem::path& path,
        std::string_view contents)
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << contents;
        if (!output) {
            throw std::runtime_error("cannot write scheduler freshness fixture");
        }
    }
};

std::string CommandTopic()
{
    return "/devices/Fan-1_2/controls/Power/on1";
}

std::string FactTopic()
{
    return "/devices/Fan-1_2/controls/Power";
}

std::string ResultTopic()
{
    return "/mdvwb/schedules/manual-off/result";
}

void InjectManualRun(FakeMqttClient& mqtt)
{
    mqtt.Inject("/mdvwb/schedules/manual-off/execute", "1", false);
}

void TestDisabledBusRejectsRunWithoutScheduleNotification()
{
    TestEnvironment environment;
    FakeClock clock;
    FakeMqttClient mqtt;
    mdvwb::SchedulerService service(mqtt, environment.paths, clock);
    service.Start();

    environment.WriteDisabledBus();
    InjectManualRun(mqtt);
    const auto result = service.ProcessOne();

    Require(result.has_value() && !result->success,
        "disabled bus was accepted from stale scheduler configuration");
    Require(mqtt.CountTopic(CommandTopic()) == 0U,
        "scheduler published a command for a disabled bus");
    const auto* publication = mqtt.LastTopic(ResultTopic());
    Require(publication != nullptr &&
            publication->payload.find("\"state\":\"rejected\"") !=
                std::string::npos,
        "disabled bus did not produce a rejected result");
}

void TestRemovedDeviceCancelsActiveRunBeforeConfirmation()
{
    TestEnvironment environment;
    FakeClock clock;
    FakeMqttClient mqtt;
    mdvwb::SchedulerService service(mqtt, environment.paths, clock);
    service.Start();

    InjectManualRun(mqtt);
    const auto accepted = service.ProcessOne();
    Require(accepted.has_value() && accepted->success && service.HasActiveRun(),
        "manual run did not start before the device removal test");
    Require(mqtt.CountTopic(CommandTopic()) == 1U,
        "manual run did not publish its Power command");

    environment.WriteDeviceRemoved();
    mqtt.Inject(FactTopic(), "0", false);
    const auto fact = service.ProcessOne();

    Require(fact.has_value() && !fact->success,
        "fresh fact was accepted after its device was removed");
    Require(!service.HasActiveRun(),
        "active run survived removal of its target device");
    const auto* publication = mqtt.LastTopic(ResultTopic());
    Require(publication != nullptr &&
            publication->payload.find("\"state\":\"failed\"") !=
                std::string::npos,
        "removed target did not fail the active run");
    Require(publication->payload.find("completed") == std::string::npos,
        "removed target incorrectly completed from a stale fact");
}

void TestDashboardRemovalRejectsRun()
{
    TestEnvironment environment;
    FakeClock clock;
    FakeMqttClient mqtt;
    mdvwb::SchedulerService service(mqtt, environment.paths, clock);
    service.Start();

    environment.WriteDashboardWithoutTarget();
    InjectManualRun(mqtt);
    const auto result = service.ProcessOne();

    Require(result.has_value() && !result->success,
        "dashboard target removal was ignored by the scheduler");
    Require(mqtt.CountTopic(CommandTopic()) == 0U,
        "scheduler used a target removed from the dashboard");
}

void TestValidDependencyRepairUnblocksScheduler()
{
    TestEnvironment environment;
    FakeClock clock;
    FakeMqttClient mqtt;
    mdvwb::SchedulerService service(mqtt, environment.paths, clock);
    service.Start();

    environment.WriteDisabledBus();
    InjectManualRun(mqtt);
    const auto rejected = service.ProcessOne();
    Require(rejected.has_value() && !rejected->success,
        "invalid dependency did not block the scheduler");

    environment.WriteValidBuses();
    InjectManualRun(mqtt);
    const auto accepted = service.ProcessOne();

    Require(accepted.has_value() && accepted->success,
        "valid dependency repair did not unblock the scheduler");
    Require(service.HasActiveRun(),
        "repaired scheduler did not start the requested run");
    Require(mqtt.CountTopic(CommandTopic()) == 1U,
        "repaired scheduler did not publish the Power command exactly once");
}

} // namespace

int main()
{
    try {
        TestDisabledBusRejectsRunWithoutScheduleNotification();
        TestRemovedDeviceCancelsActiveRunBeforeConfirmation();
        TestDashboardRemovalRejectsRun();
        TestValidDependencyRepairUnblocksScheduler();
        std::cout << "mdvwb_scheduler_freshness_test: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "mdvwb_scheduler_freshness_test: " << error.what() << '\n';
        return 1;
    }
}
