#pragma once

#include "mdv_mqtt.h"

#include <cstdint>
#include <string_view>
#include <vector>

namespace mdv {

// Publishes the retained Wiren Board device/control metadata required for the
// standard Devices page. The driver owns these topics, so a separate wb-rules
// ArrID script is no longer required.
class MqttMetadataPublisher final {
public:
    MqttMetadataPublisher(int busNumber, IMqttClient& client);

    void Publish(const std::vector<std::uint8_t>& addresses);

private:
    void PublishDevice(std::uint8_t address);
    void PublishSystemDevice();
    void PublishControl(
        std::string_view device,
        std::string_view control,
        std::string_view title,
        std::string_view type,
        int order,
        bool readonly,
        std::string_view maximum = {});
    void PublishRetained(std::string topic, std::string_view payload);

    int busNumber_ = 0;
    IMqttClient& client_;
};

} // namespace mdv
