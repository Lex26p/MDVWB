#include "mdvwb_manager_mqtt.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void WriteFile(
    const std::filesystem::path& path,
    std::string_view content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write discovery isolation fixture");
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
            ("mdvwb-discovery-isolation-" + std::to_string(token) + "-" +
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

    void Inject(
        std::string topic,
        std::string payload,
        bool retained = false)
    {
        Require(
            static_cast<bool>(handler_),
            "MQTT handler is not installed");
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

class RecordingCommandRunner final : public mdvwb::CommandRunner {
public:
    int Run(const std::vector<std::string>& arguments) override
    {
        commands.push_back(arguments);
        if (arguments.size() >= 2U &&
            (arguments[1] == "is-active" ||
             arguments[1] == "is-enabled")) {
            return 0;
        }
        return 0;
    }

    std::vector<std::vector<std::string>> commands;
};

class ConcurrentBlockingDiscoveryRunner final : public mdvwb::DiscoveryRunner {
public:
    mdvwb::DiscoveryExecutionResult Run(
        std::string_view port,
        int masterId,
        int periodMilliseconds,
        int responseTimeoutMilliseconds) override
    {
        const std::string key(port);
        {
            std::lock_guard lock(mutex_);
            ++calls_[key];
            arguments_[key] = {
                masterId,
                periodMilliseconds,
                responseTimeoutMilliseconds};
            started_.insert(key);
            ++activeCalls_;
            maxActiveCalls_ = std::max(maxActiveCalls_, activeCalls_);
        }
        condition_.notify_all();

        std::unique_lock lock(mutex_);
        const bool released = condition_.wait_for(
            lock,
            std::chrono::seconds(3),
            [&] {
                return released_.contains(key);
            });
        --activeCalls_;
        lock.unlock();
        condition_.notify_all();

        if (!released) {
            return {
                false,
                124,
                {},
                {},
                "Test discovery runner timed out waiting for release"};
        }
        if (key == "/dev/ttyRS485-1") {
            return {
                true,
                0,
                {1, 3, 18},
                "FOUND_ADDRESSES=1,3,18\n",
                "Discovery completed"};
        }
        return {
            true,
            0,
            {2, 4},
            "FOUND_ADDRESSES=2,4\n",
            "Discovery completed"};
    }

    [[nodiscard]] bool WaitUntilStarted(
        std::string_view port,
        std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock,
            timeout,
            [&] {
                return started_.contains(std::string(port));
            });
    }

    void Release(std::string_view port)
    {
        {
            std::lock_guard lock(mutex_);
            released_.insert(std::string(port));
        }
        condition_.notify_all();
    }

    void ReleaseAll() noexcept
    {
        {
            std::lock_guard lock(mutex_);
            for (const auto& port : started_) {
                released_.insert(port);
            }
        }
        condition_.notify_all();
    }

    [[nodiscard]] int Calls(std::string_view port) const
    {
        std::lock_guard lock(mutex_);
        const auto iterator = calls_.find(std::string(port));
        return iterator == calls_.end() ? 0 : iterator->second;
    }

    [[nodiscard]] int MaxActiveCalls() const
    {
        std::lock_guard lock(mutex_);
        return maxActiveCalls_;
    }

    [[nodiscard]] bool HasExpectedArguments(std::string_view port) const
    {
        std::lock_guard lock(mutex_);
        const auto iterator = arguments_.find(std::string(port));
        return iterator != arguments_.end() &&
            iterator->second == std::tuple<int, int, int>{0, 150, 130};
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::set<std::string> started_;
    std::set<std::string> released_;
    std::map<std::string, int> calls_;
    std::map<std::string, std::tuple<int, int, int>> arguments_;
    int activeCalls_ = 0;
    int maxActiveCalls_ = 0;
};

class DiscoveryReleaseGuard final {
public:
    explicit DiscoveryReleaseGuard(
        ConcurrentBlockingDiscoveryRunner& runner) noexcept
        : runner_(runner)
    {
    }

    ~DiscoveryReleaseGuard()
    {
        runner_.ReleaseAll();
    }

    DiscoveryReleaseGuard(const DiscoveryReleaseGuard&) = delete;
    DiscoveryReleaseGuard& operator=(const DiscoveryReleaseGuard&) = delete;

private:
    ConcurrentBlockingDiscoveryRunner& runner_;
};

bool HasCommand(
    const RecordingCommandRunner& runner,
    const std::vector<std::string>& expected)
{
    for (const auto& command : runner.commands) {
        if (command == expected) {
            return true;
        }
    }
    return false;
}

std::optional<mdvwb::ManagerMqttResult> WaitForDiscoveryCompletion(
    mdvwb::ManagerMqttService& service,
    int busId)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        auto result = service.ProcessOne();
        if (result.has_value() &&
            result->command == "discovery" &&
            result->busId == busId &&
            result->message == "Discovery completed") {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
}

void TestDiscoveryIsSerializedPerBusAndParallelAcrossBuses()
{
    TemporaryDirectory temporary;
    const std::filesystem::path config =
        temporary.Path() / "etc/mdvwb/buses.json";
    const std::filesystem::path dashboard =
        temporary.Path() / "etc/mdvwb/dashboard.json";
    const std::filesystem::path schedules =
        temporary.Path() / "etc/mdvwb/schedules.json";

    WriteFile(
        config,
        R"json({
          "version":1,
          "revision":0,
          "buses":[
            {
              "id":1,
              "enabled":true,
              "port":"/dev/ttyRS485-1",
              "addresses":[1]
            },
            {
              "id":2,
              "enabled":true,
              "port":"/dev/ttyRS485-2",
              "addresses":[2]
            }
          ]
        })json");

    mdvwb::ServiceSyncPaths paths;
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

    FakeMqttClient mqtt;
    RecordingCommandRunner commands;
    ConcurrentBlockingDiscoveryRunner discovery;
    mdvwb::ManagerMqttService service(
        mqtt,
        config,
        paths,
        commands,
        &discovery,
        dashboard,
        temporary.Path() / "assets",
        schedules);
    service.Start();
    DiscoveryReleaseGuard releaseGuard(discovery);

    mqtt.Inject("/mdvwb/buses/1/discovery/start", "1");
    const auto firstStart = service.ProcessOne();
    Require(
        firstStart.has_value() && firstStart->success &&
            firstStart->busId == 1 &&
            firstStart->message.find("background") != std::string::npos,
        "first bus discovery was not accepted");
    Require(
        discovery.WaitUntilStarted(
            "/dev/ttyRS485-1", std::chrono::milliseconds(500)),
        "first bus discovery worker did not start");

    mqtt.Inject("/mdvwb/buses/1/discovery/start", "1");
    const auto duplicate = service.ProcessOne();
    Require(
        duplicate.has_value() && !duplicate->success &&
            duplicate->busId == 1 &&
            duplicate->message.find("this bus") != std::string::npos,
        "second discovery on the same bus was not rejected");
    Require(
        discovery.Calls("/dev/ttyRS485-1") == 1,
        "duplicate discovery invoked the same bus runner twice");

    mqtt.Inject("/mdvwb/buses/2/discovery/start", "1");
    const auto secondStart = service.ProcessOne();
    Require(
        secondStart.has_value() && secondStart->success &&
            secondStart->busId == 2 &&
            secondStart->message.find("background") != std::string::npos,
        "discovery on a different bus was not accepted");
    Require(
        discovery.WaitUntilStarted(
            "/dev/ttyRS485-2", std::chrono::milliseconds(500)),
        "second bus discovery worker did not start");
    Require(
        discovery.MaxActiveCalls() >= 2,
        "different bus discoveries did not run independently");
    Require(
        discovery.HasExpectedArguments("/dev/ttyRS485-1") &&
            discovery.HasExpectedArguments("/dev/ttyRS485-2"),
        "parallel discovery received wrong protocol arguments");

    Require(
        HasCommand(
            commands,
            {"fake-systemctl", "stop", "mdvwb@1.service"}) &&
            HasCommand(
                commands,
                {"fake-systemctl", "stop", "mdvwb@2.service"}),
        "discovery did not stop each selected bus service");

    mqtt.Inject("/mdvwb/buses/1/status/get", "1");
    const auto status = service.ProcessOne();
    Require(
        status.has_value() && status->success && status->command == "status",
        "manager stopped processing commands during parallel discovery");

    discovery.Release("/dev/ttyRS485-2");
    const auto secondCompletion = WaitForDiscoveryCompletion(service, 2);
    Require(
        secondCompletion.has_value() && secondCompletion->success,
        "second bus completion was not processed independently");
    Require(
        discovery.Calls("/dev/ttyRS485-1") == 1,
        "processing bus 2 completion restarted bus 1 discovery");

    const auto* secondResult = mqtt.Last(
        "/mdvwb/buses/2/discovery/result");
    Require(
        secondResult != nullptr && secondResult->retained &&
            secondResult->payload.find("\"addresses\":[2,4]") !=
                std::string::npos,
        "second bus completion published wrong addresses");

    discovery.Release("/dev/ttyRS485-1");
    const auto firstCompletion = WaitForDiscoveryCompletion(service, 1);
    Require(
        firstCompletion.has_value() && firstCompletion->success,
        "first bus completion was not processed");

    const auto* firstResult = mqtt.Last(
        "/mdvwb/buses/1/discovery/result");
    Require(
        firstResult != nullptr && firstResult->retained &&
            firstResult->payload.find("\"addresses\":[1,3,18]") !=
                std::string::npos,
        "first bus completion published wrong addresses");

    const auto* firstStatus = mqtt.Last(
        "/mdvwb/buses/1/discovery/status");
    const auto* secondStatus = mqtt.Last(
        "/mdvwb/buses/2/discovery/status");
    Require(
        firstStatus != nullptr && secondStatus != nullptr &&
            firstStatus->payload.find("\"state\":\"completed\"") !=
                std::string::npos &&
            secondStatus->payload.find("\"state\":\"completed\"") !=
                std::string::npos,
        "parallel discovery did not publish both completed statuses");
}

} // namespace

int main()
{
    try {
        TestDiscoveryIsSerializedPerBusAndParallelAcrossBuses();
        std::cout << "MDVWB discovery async tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB discovery async tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
