#include "mdv_metadata.h"

#include <stdexcept>
#include <string>

namespace mdv {

MqttMetadataPublisher::MqttMetadataPublisher(int busNumber, IMqttClient& client)
    : busNumber_(busNumber), client_(client)
{
    if (busNumber_ < 1 || busNumber_ > 999) {
        throw std::invalid_argument("MQTT bus number must be in range 1..999");
    }
}

void MqttMetadataPublisher::Publish(const std::vector<std::uint8_t>& addresses)
{
    PublishSystemDevice();
    for (const std::uint8_t address : addresses) {
        PublishDevice(address);
    }
}

void MqttMetadataPublisher::PublishDevice(std::uint8_t address)
{
    const std::string device =
        "Fan-" + std::to_string(busNumber_) + "_" + std::to_string(address);
    const std::string prefix = "/devices/" + device;

    PublishRetained(
        prefix + "/meta/name",
        "Кондиционер " + std::to_string(busNumber_) + "-" + std::to_string(address));
    PublishRetained(prefix + "/meta/driver", "MDVWB");

    PublishControl(device, "Alarm", "Авария", "value", 1, true, "500");
    PublishControl(device, "AlarmCode", "Код аварии", "value", 2, true, "500");
    PublishControl(device, "Blinds", "Жалюзи", "value", 3, true, "500");
    PublishControl(device, "Blok", "Блокировка", "value", 4, true, "500");
    PublishControl(device, "Mode", "Режим", "value", 5, true, "500");
    PublishControl(device, "Power", "Питание", "value", 6, true, "500");
    PublishControl(device, "SetTemp", "Уставка", "value", 7, true, "500");
    PublishControl(device, "Speed", "Скорость", "value", 8, true, "500");
    PublishControl(device, "Status", "Статус", "value", 9, true, "500");
    PublishControl(device, "Temp", "Температура", "value", 10, true, "500");
}

void MqttMetadataPublisher::PublishSystemDevice()
{
    const std::string device = "sist-" + std::to_string(busNumber_);
    const std::string prefix = "/devices/" + device;
    PublishRetained(
        prefix + "/meta/name",
        "Статус опроса кондиционеров " + std::to_string(busNumber_));
    PublishRetained(prefix + "/meta/driver", "MDVWB");
    PublishControl(device, "Serial", "Последовательный порт", "text", 1, true);
    PublishControl(device, "Error", "Ошибка", "text", 2, true);
    PublishControl(device, "GanGetID", "Текущий адрес", "value", 3, true, "500");
}

void MqttMetadataPublisher::PublishControl(
    std::string_view device,
    std::string_view control,
    std::string_view title,
    std::string_view type,
    int order,
    bool readonly,
    std::string_view maximum)
{
    const std::string prefix = "/devices/" + std::string(device) +
        "/controls/" + std::string(control) + "/meta/";
    PublishRetained(prefix + "title", title);
    PublishRetained(prefix + "type", type);
    PublishRetained(prefix + "order", std::to_string(order));
    PublishRetained(prefix + "readonly", readonly ? "1" : "0");
    if (!maximum.empty()) {
        PublishRetained(prefix + "max", maximum);
    }
}

void MqttMetadataPublisher::PublishRetained(
    std::string topic,
    std::string_view payload)
{
    client_.Publish(topic, payload, true);
}

} // namespace mdv
