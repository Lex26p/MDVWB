#include "mdv_modbus_profile_mqtt.h"

#include "mdv_modbus_profile_ui.h"

#include <exception>
#include <string>

namespace mdvwb {
namespace {

constexpr std::string_view kUnavailableCatalog =
    R"json({"schemaVersion":1,"profiles":[],"issues":[{"file":"catalog","message":"Modbus profile catalog is unavailable"}]})json";

} // namespace

ModbusProfileUiPublishResult PublishModbusProfileUiCatalog(
    mdv::IMqttClient& client,
    const std::filesystem::path& profileDirectory)
{
    ModbusProfileUiPublishResult result;
    std::string payload;

    try {
        payload = LoadModbusProfileUiCatalog(profileDirectory);
        result.catalogLoaded = true;
    }
    catch (const std::exception& error) {
        payload = std::string(kUnavailableCatalog);
        result.warning = error.what();
    }

    result.status = client.PublishWithResult(
        kModbusProfileUiCatalogTopic,
        payload,
        true);
    return result;
}

} // namespace mdvwb
