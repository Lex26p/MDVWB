#include "mdv_modbus_profile_mqtt.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifndef MDVWB_SOURCE_DIR
#error MDVWB_SOURCE_DIR must point to the repository source directory
#endif

namespace {

void Require(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

class RecordingMqttClient final : public mdv::IMqttClient {
public:
    void SetMessageHandler(MessageHandler handler) override
    {
        handler_ = std::move(handler);
    }

    void Subscribe(std::string_view topicFilter) override
    {
        subscription_ = std::string(topicFilter);
    }

    void Publish(
        std::string_view topic,
        std::string_view payload,
        bool retained) override
    {
        topic_ = std::string(topic);
        payload_ = std::string(payload);
        retained_ = retained;
    }

    [[nodiscard]] mdv::MqttPublishStatus PublishWithResult(
        std::string_view topic,
        std::string_view payload,
        bool retained) override
    {
        Publish(topic, payload, retained);
        return nextStatus;
    }

    mdv::MqttPublishStatus nextStatus = mdv::MqttPublishStatus::Published;
    std::string topic_;
    std::string payload_;
    std::string subscription_;
    bool retained_ = false;
    MessageHandler handler_;
};

void TestProductionCatalogIsPublishedRetained()
{
    RecordingMqttClient client;
    const auto result = mdvwb::PublishModbusProfileUiCatalog(
        client,
        std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus");

    Require(result.catalogLoaded, "production profile catalog was not loaded");
    Require(result.warning.empty(), "valid profile catalog returned a warning");
    Require(
        result.status == mdv::MqttPublishStatus::Published,
        "publication status was not propagated");
    Require(
        client.topic_ == mdvwb::kModbusProfileUiCatalogTopic,
        "profile catalog used the wrong MQTT topic");
    Require(client.retained_, "profile catalog was not retained");
    Require(
        client.payload_.find("\"schemaVersion\":1") != std::string::npos,
        "profile catalog omitted its schema version");
    Require(
        client.payload_.find("\"id\":\"vrf_add_controller\"") !=
            std::string::npos,
        "profile catalog omitted the production profile");
    Require(
        client.payload_.find("\"power\":{\"supported\":true") !=
            std::string::npos,
        "profile catalog omitted Power capability metadata");
}

void TestUnavailableDirectoryPublishesSafeFallback()
{
    RecordingMqttClient client;
    const auto missing =
        std::filesystem::temp_directory_path() /
        "mdvwb-profile-ui-catalog-directory-that-does-not-exist";
    std::error_code cleanupError;
    std::filesystem::remove_all(missing, cleanupError);

    const auto result =
        mdvwb::PublishModbusProfileUiCatalog(client, missing);

    Require(!result.catalogLoaded, "missing profile directory was reported as loaded");
    Require(!result.warning.empty(), "missing profile directory lost its diagnostic");
    Require(client.retained_, "fallback profile catalog was not retained");
    Require(
        client.payload_ ==
            "{\"schemaVersion\":1,\"profiles\":[],\"issues\":[{\"file\":\"catalog\",\"message\":\"Modbus profile catalog is unavailable\"}]}",
        "missing profile directory did not produce the stable safe fallback");
    Require(
        client.payload_.find(missing.string()) == std::string::npos,
        "fallback payload leaked a server path");
}



void TestRetainedPublisherStopsPromptly()
{
    const auto started = std::chrono::steady_clock::now();
    {
        mdvwb::ModbusProfileUiRetainedPublisher publisher(
            std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus");
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    Require(
        elapsed < std::chrono::seconds(5),
        "retained profile publisher did not stop promptly");
}

void TestTransportStatusIsNotHidden()
{
    RecordingMqttClient client;
    client.nextStatus = mdv::MqttPublishStatus::Failed;

    const auto result = mdvwb::PublishModbusProfileUiCatalog(
        client,
        std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus");

    Require(
        result.status == mdv::MqttPublishStatus::Failed,
        "profile publisher hid the transport failure status");
}

} // namespace

int main()
{
    try {
        TestProductionCatalogIsPublishedRetained();
        TestUnavailableDirectoryPublishesSafeFallback();
        TestTransportStatusIsNotHidden();
        TestRetainedPublisherStopsPromptly();
        std::cout << "MDVWB Modbus profile MQTT publication tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus profile MQTT publication tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
