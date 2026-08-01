#pragma once

#include "mdv_mqtt.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>

namespace mdvwb {

inline constexpr std::string_view kModbusProfileUiCatalogTopic =
    "/mdvwb/modbus/profiles";

struct ModbusProfileUiPublishResult {
    bool catalogLoaded = false;
    mdv::MqttPublishStatus status = mdv::MqttPublishStatus::Failed;
    std::string warning;
};

// Publishes a retained, web-safe profile catalog through an already-owned MQTT
// client. A missing/unreadable directory is represented by a safe empty catalog
// and returned as a diagnostic instead of leaking server paths into the payload.
[[nodiscard]] ModbusProfileUiPublishResult PublishModbusProfileUiCatalog(
    mdv::IMqttClient& client,
    const std::filesystem::path& profileDirectory);

// Manager-owned background publisher. It keeps the retained catalog queued
// across an unavailable broker without delaying startup of the main manager
// endpoint, and exits after the transport accepts the retained publication.
class ModbusProfileUiRetainedPublisher final {
public:
    explicit ModbusProfileUiRetainedPublisher(
        std::filesystem::path profileDirectory);
    ~ModbusProfileUiRetainedPublisher();

    ModbusProfileUiRetainedPublisher(
        const ModbusProfileUiRetainedPublisher&) = delete;
    ModbusProfileUiRetainedPublisher& operator=(
        const ModbusProfileUiRetainedPublisher&) = delete;

private:
    std::atomic_bool stopRequested_{false};
    std::thread worker_;
};

} // namespace mdvwb
