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
            ("mdvwb-dashboard-concurrency-" + std::to_string(token) + "-" +
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
        Require(static_cast<bool>(handler_), "MQTT handler is not installed");
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

    void ClearPublications()
    {
        publications.clear();
    }

    MessageHandler handler_;
    std::vector<std::string> subscriptions;
    std::vector<mdv::MqttPublication> publications;
};

class StatusRunner final : public mdvwb::CommandRunner {
public:
    int Run(const std::vector<std::string>& arguments) override
    {
        if (arguments.size() >= 2U && arguments[1] == "is-active") {
            return 0;
        }
        if (arguments.size() >= 2U && arguments[1] == "is-enabled") {
            return 0;
        }
        return 0;
    }
};

void WriteFile(
    const std::filesystem::path& path,
    std::string_view content)
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

std::string PngHeader(int width, int height)
{
    std::string data(32, '\0');
    const unsigned char signature[] = {
        0x89U, 0x50U, 0x4eU, 0x47U,
        0x0dU, 0x0aU, 0x1aU, 0x0aU};
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

std::string DashboardPayload(int revision, std::string_view title)
{
    return std::string(R"json({
  "version":2,
  "revision":)json") +
        std::to_string(revision) + R"json(,
  "defaultPanel":"main",
  "panels":[{
    "id":"main",
    "title":")json" + std::string(title) + R"json(",
    "background":{
      "file":"",
      "naturalWidth":0,
      "naturalHeight":0,
      "defaultScale":1,
      "fit":"contain"
    },
    "fans":[{
      "id":"fan-1-1",
      "number":1,
      "bus":1,
      "address":1,
      "label":"Fan 1",
      "x":0.5,
      "y":0.5,
      "markerScale":1,
      "rotation":0,
      "visible":true
    }]
  }]
})json";
}

struct Fixture {
    TemporaryDirectory temporary;
    std::filesystem::path buses =
        temporary.Path() / "etc/mdvwb/buses.json";
    std::filesystem::path dashboard =
        temporary.Path() / "etc/mdvwb/dashboard.json";
    std::filesystem::path schedules =
        temporary.Path() / "etc/mdvwb/schedules.json";
    std::filesystem::path assets = temporary.Path() / "assets";
    mdvwb::ServiceSyncPaths paths;
    FakeMqttClient mqtt;
    StatusRunner runner;

    explicit Fixture(int dashboardRevision = 4)
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
        WriteFile(
            buses,
            R"json({
  "version":1,
  "revision":0,
  "buses":[{
    "id":1,
    "enabled":true,
    "port":"/dev/ttyRS485-1",
    "addresses":[1]
  }]
})json");
        WriteFile(
            dashboard,
            DashboardPayload(dashboardRevision, "Current"));
        WriteFile(
            schedules,
            R"json({"version":1,"revision":0,"schedules":[]})json");
    }

    mdvwb::ManagerMqttService CreateService()
    {
        return mdvwb::ManagerMqttService(
            mqtt,
            buses,
            paths,
            runner,
            nullptr,
            dashboard,
            assets,
            schedules);
    }
};

std::size_t FindPublicationIndex(
    const FakeMqttClient& mqtt,
    std::string_view topic,
    std::size_t begin)
{
    for (std::size_t index = begin; index < mqtt.publications.size(); ++index) {
        if (mqtt.publications[index].topic == topic) {
            return index;
        }
    }
    throw std::runtime_error(
        "publication was not found: " + std::string(topic));
}

void RequireResultRevision(
    const FakeMqttClient& mqtt,
    std::string_view topic,
    int revision,
    std::string_view message)
{
    const mdv::MqttPublication* publication = mqtt.Last(topic);
    Require(publication != nullptr && !publication->retained, message);
    Require(
        publication->payload.find(
            "\"revision\":" + std::to_string(revision)) !=
            std::string::npos,
        message);
}

void TestInitialRevisionIsExplicitInUploadResult()
{
    Fixture fixture(0);
    auto service = fixture.CreateService();
    service.Start();
    fixture.mqtt.ClearPublications();

    fixture.mqtt.Inject(
        "/mdvwb/dashboard/background/upload/finish/not_started",
        "1");
    const auto result = service.ProcessOne();

    Require(result.has_value() && !result->success && !result->saved,
        "finish without an active upload was accepted");
    RequireResultRevision(
        fixture.mqtt,
        mdvwb::ManagerMqttService::BackgroundUploadResultTopic,
        0,
        "terminal upload result omitted initial revision zero");
}

void TestRejectedSaveReportsCurrentRevision()
{
    Fixture fixture;
    auto service = fixture.CreateService();
    service.Start();
    fixture.mqtt.ClearPublications();
    const std::string original = ReadFile(fixture.dashboard);

    fixture.mqtt.Inject(
        mdvwb::ManagerMqttService::DashboardConfigSetTopic,
        R"json({"version":2,"revision":4,"defaultPanel":"main","panels":[]})json");
    const auto result = service.ProcessOne();

    Require(result.has_value() && !result->success && !result->saved,
        "invalid dashboard save was accepted");
    Require(ReadFile(fixture.dashboard) == original,
        "invalid dashboard save changed the file");
    RequireResultRevision(
        fixture.mqtt,
        mdvwb::ManagerMqttService::DashboardConfigResultTopic,
        4,
        "invalid dashboard result did not report current revision");
}

void TestSuccessfulSaveReportsCommittedRevision()
{
    Fixture fixture;
    auto service = fixture.CreateService();
    service.Start();
    fixture.mqtt.ClearPublications();

    fixture.mqtt.Inject(
        mdvwb::ManagerMqttService::DashboardConfigSetTopic,
        DashboardPayload(4, "Saved"));
    const auto result = service.ProcessOne();

    Require(result.has_value() && result->success && result->saved,
        "valid dashboard save failed");
    const std::string saved = ReadFile(fixture.dashboard);
    Require(saved.find("\"revision\": 5") != std::string::npos,
        "dashboard save did not increment revision");
    Require(saved.find("\"title\": \"Saved\"") != std::string::npos,
        "dashboard save did not persist the new title");
    RequireResultRevision(
        fixture.mqtt,
        mdvwb::ManagerMqttService::DashboardConfigResultTopic,
        5,
        "successful dashboard result did not report committed revision");
}

void StartUpload(
    Fixture& fixture,
    mdvwb::ManagerMqttService& service,
    std::string_view uploadId,
    const std::string& image,
    std::string_view sha256,
    int revision)
{
    const std::string startPayload =
        "{\"version\":1,\"uploadId\":\"" + std::string(uploadId) +
        "\",\"fileName\":\"floor.png\",\"panelId\":\"main\",\"size\":" +
        std::to_string(image.size()) + ",\"sha256\":\"" +
        std::string(sha256) + "\",\"revision\":" +
        std::to_string(revision) + "}";
    fixture.mqtt.Inject(
        mdvwb::ManagerMqttService::BackgroundUploadStartTopic,
        startPayload);
    auto result = service.ProcessOne();
    Require(result.has_value() && result->success && !result->saved,
        "background upload did not start");
    RequireResultRevision(
        fixture.mqtt,
        mdvwb::ManagerMqttService::BackgroundUploadResultTopic,
        revision,
        "upload start result did not report current revision");

    fixture.mqtt.Inject(
        "/mdvwb/dashboard/background/upload/chunk/" +
            std::string(uploadId) + "/0",
        image);
    result = service.ProcessOne();
    Require(result.has_value() && result->success,
        "background upload chunk failed");
}

void TestFailedUploadCompletionReportsCurrentRevision()
{
    Fixture fixture;
    auto service = fixture.CreateService();
    service.Start();
    fixture.mqtt.ClearPublications();

    const std::string image = PngHeader(640, 480);
    StartUpload(
        fixture,
        service,
        "bad_sha",
        image,
        std::string(64U, '0'),
        4);

    fixture.mqtt.Inject(
        "/mdvwb/dashboard/background/upload/finish/bad_sha",
        "1");
    const auto result = service.ProcessOne();

    Require(result.has_value() && !result->success && !result->saved,
        "upload with invalid digest was accepted");
    RequireResultRevision(
        fixture.mqtt,
        mdvwb::ManagerMqttService::BackgroundUploadResultTopic,
        4,
        "failed upload completion did not report current revision");
    Require(
        mdvwb::LoadDashboardCollection(fixture.dashboard).revision == 4,
        "failed upload changed dashboard revision");
}

void TestConcurrentSaveInvalidatesUploadAtCurrentRevision()
{
    Fixture fixture;
    auto service = fixture.CreateService();
    service.Start();
    fixture.mqtt.ClearPublications();

    const std::string image = PngHeader(800, 600);
    const std::string sha256 = mdvwb::ComputeSha256Hex(image);
    StartUpload(fixture, service, "concurrent", image, sha256, 4);

    fixture.mqtt.Inject(
        mdvwb::ManagerMqttService::DashboardConfigSetTopic,
        DashboardPayload(4, "Other editor"));
    auto result = service.ProcessOne();
    Require(result.has_value() && result->success && result->saved,
        "concurrent dashboard save failed");

    const std::size_t publicationCountBeforeFinish =
        fixture.mqtt.publications.size();
    fixture.mqtt.Inject(
        "/mdvwb/dashboard/background/upload/finish/concurrent",
        "1");
    result = service.ProcessOne();

    Require(result.has_value() && !result->success && !result->saved,
        "upload completed against a stale dashboard revision");
    Require(result->message.find("revision conflict") != std::string::npos,
        "stale upload completion did not explain revision conflict");
    RequireResultRevision(
        fixture.mqtt,
        mdvwb::ManagerMqttService::BackgroundUploadResultTopic,
        5,
        "stale upload completion did not report current revision");
    const std::size_t resultIndex = FindPublicationIndex(
        fixture.mqtt,
        mdvwb::ManagerMqttService::BackgroundUploadResultTopic,
        publicationCountBeforeFinish);
    const std::size_t configIndex = FindPublicationIndex(
        fixture.mqtt,
        mdvwb::ManagerMqttService::DashboardConfigTopic,
        publicationCountBeforeFinish);
    Require(resultIndex < configIndex,
        "upload conflict result must precede the refreshed dashboard");
    Require(
        fixture.mqtt.publications[configIndex].retained &&
            fixture.mqtt.publications[configIndex].payload.find(
                "\"revision\": 5") != std::string::npos,
        "upload conflict did not republish current dashboard revision");

    const std::string saved = ReadFile(fixture.dashboard);
    Require(saved.find("\"revision\": 5") != std::string::npos &&
            saved.find("\"title\": \"Other editor\"") != std::string::npos,
        "stale upload completion changed the concurrent dashboard save");
    Require(saved.find("background-") == std::string::npos,
        "stale upload completion committed a background file");
}

}  // namespace

int main()
{
    try {
        TestInitialRevisionIsExplicitInUploadResult();
        TestRejectedSaveReportsCurrentRevision();
        TestSuccessfulSaveReportsCommittedRevision();
        TestFailedUploadCompletionReportsCurrentRevision();
        TestConcurrentSaveInvalidatesUploadAtCurrentRevision();
        std::cout << "MDVWB dashboard concurrency tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB dashboard concurrency tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
