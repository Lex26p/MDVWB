#include "mdvwb_manager_mqtt.h"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void WriteFile(
    const std::filesystem::path& path,
    std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write discovery async fixture");
    }
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static unsigned long long counter = 0;
        const auto token = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-discovery-async-" + std::to_string(token) + "-" +
             std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return path_;
    }

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
        publications.push_back(
            {std::string(topic), std::string(payload), retained});
    }

    void Inject(
        std::string topic,
        std::string payload,
        bool retained = false) {
        Require(
            static_cast<bool>(handler_),
            "MQTT handler is not installed");
        handler_({
            std::move(topic),
            std::move(payload),
            retained});
    }

    [[nodiscard]] const mdv::MqttPublication* Last(
        std::string_view topic) const {
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
    int Run(const std::vector<std::string>& arguments) override {
        commands.push_back(arguments);
        if (arguments.size() >= 2U &&
            arguments[1] == "is-active") {
            return 0;
        }
        if (arguments.size() >= 2U &&
            arguments[1] == "is-enabled") {
            return 0;
        }
        return 0;
    }

    std::vector<std::vector<std::string>> commands;
};

class BlockingDiscoveryRunner final : public mdvwb::DiscoveryRunner {
public:
    mdvwb::DiscoveryExecutionResult Run(
        std::string_view port,
        int masterId,
        int periodMilliseconds,
        int responseTimeoutMilliseconds) override {
        {
            std::lock_guard lock(mutex_);
            ++calls_;
            port_ = std::string(port);
            masterId_ = masterId;
            periodMilliseconds_ = periodMilliseconds;
            responseTimeoutMilliseconds_ =
                responseTimeoutMilliseconds;
            started_ = true;
        }
        condition_.notify_all();

        std::unique_lock lock(mutex_);
        const bool released = condition_.wait_for(
            lock,
            std::chrono::seconds(3),
            [this] {
                return released_;
            });
        if (!released) {
            return {
                false,
                124,
                {},
                {},
                "Test discovery runner timed out waiting for release"};
        }
        return {
            true,
            0,
            {1, 3, 18},
            "FOUND_ADDRESSES=1,3,18\n",
            "Discovery completed"};
    }

    bool WaitUntilStarted(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(
            lock,
            timeout,
            [this] {
                return started_;
            });
    }

    void Release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        condition_.notify_all();
    }

    [[nodiscard]] int Calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    [[nodiscard]] bool HasExpectedArguments() const {
        std::lock_guard lock(mutex_);
        return port_ == "/dev/ttyRS485-1" &&
            masterId_ == 0 &&
            periodMilliseconds_ == 150 &&
            responseTimeoutMilliseconds_ == 130;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool started_ = false;
    bool released_ = false;
    int calls_ = 0;
    std::string port_;
    int masterId_ = -1;
    int periodMilliseconds_ = -1;
    int responseTimeoutMilliseconds_ = -1;
};

class DiscoveryReleaseGuard final {
public:
    explicit DiscoveryReleaseGuard(BlockingDiscoveryRunner& runner) noexcept
        : runner_(runner) {
    }

    ~DiscoveryReleaseGuard() {
        runner_.Release();
    }

    DiscoveryReleaseGuard(const DiscoveryReleaseGuard&) = delete;
    DiscoveryReleaseGuard& operator=(const DiscoveryReleaseGuard&) = delete;

private:
    BlockingDiscoveryRunner& runner_;
};

bool HasCommand(
    const RecordingCommandRunner& runner,
    const std::vector<std::string>& expected) {
    for (const auto& command : runner.commands) {
        if (command == expected) {
            return true;
        }
    }
    return false;
}

void TestDiscoveryDoesNotBlockManagerAndRejectsSecondRun() {
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
    paths.environmentTemplate =
        temporary.Path() / "mdvwb.env";
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
    BlockingDiscoveryRunner discovery;
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

    // Install the release guard before starting discovery. Even if the first
    // ProcessOne() throws, the fake worker is released before the manager
    // destructor joins its thread. The runner also has its own three-second
    // safety deadline, so this test can fail but can never hang indefinitely.
    DiscoveryReleaseGuard releaseGuard(discovery);
    mqtt.Inject(
        "/mdvwb/buses/1/discovery/start",
        "1");
    const auto startResult = service.ProcessOne();

    Require(
        startResult.has_value() &&
            startResult->success &&
            startResult->command == "discovery",
        "discovery start was not accepted");
    Require(
        startResult->message.find("background") !=
            std::string::npos,
        "long discovery did not switch to background execution");
    // The fake runner remains blocked until Release(). Returning from
    // ProcessOne() before that release proves that discovery does not run
    // synchronously on the manager thread; a wall-clock threshold would only
    // make this test sensitive to slow Debug/CI machines.
    Require(
        discovery.WaitUntilStarted(
            std::chrono::milliseconds(500)),
        "discovery worker did not start");
    Require(
        discovery.HasExpectedArguments(),
        "discovery worker received wrong protocol arguments");
    Require(
        HasCommand(
            commands,
            {"fake-systemctl", "stop", "mdvwb@1.service"}),
        "discovery did not stop the selected bus service");

    mqtt.Inject(
        "/mdvwb/buses/2/discovery/start",
        "1");
    const auto duplicateResult = service.ProcessOne();
    Require(
        duplicateResult.has_value() &&
            !duplicateResult->success &&
            duplicateResult->message.find("already running") !=
                std::string::npos,
        "second simultaneous discovery was not rejected");
    Require(
        discovery.Calls() == 1,
        "second discovery invoked the runner");

    mqtt.Inject(
        "/mdvwb/buses/1/status/get",
        "1");
    const auto statusResult = service.ProcessOne();
    Require(
        statusResult.has_value() &&
            statusResult->success &&
            statusResult->command == "status",
        "manager stopped processing commands during discovery");

    discovery.Release();

    std::optional<mdvwb::ManagerMqttResult> completion;
    for (int attempt = 0; attempt < 200; ++attempt) {
        completion = service.ProcessOne();
        if (completion.has_value() &&
            completion->command == "discovery" &&
            completion->message == "Discovery completed") {
            break;
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds(5));
    }

    Require(
        completion.has_value() &&
            completion->success &&
            completion->busId == 1,
        "completed discovery result was not returned to the main loop");

    const auto* discoveryStatus = mqtt.Last(
        "/mdvwb/buses/1/discovery/status");
    Require(
        discoveryStatus != nullptr &&
            discoveryStatus->retained &&
            discoveryStatus->payload.find(
                "\"state\":\"completed\"") !=
                std::string::npos &&
            discoveryStatus->payload.find(
                "\"found\":3") !=
                std::string::npos,
        "completed discovery status is wrong");

    const auto* discoveryResult = mqtt.Last(
        "/mdvwb/buses/1/discovery/result");
    Require(
        discoveryResult != nullptr &&
            discoveryResult->retained &&
            discoveryResult->payload.find(
                "\"addresses\":[1,3,18]") !=
                std::string::npos,
        "completed discovery addresses are wrong");
}

}  // namespace

int main() {
    try {
        TestDiscoveryDoesNotBlockManagerAndRejectsSecondRun();
        std::cout << "MDVWB discovery async tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB discovery async tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
