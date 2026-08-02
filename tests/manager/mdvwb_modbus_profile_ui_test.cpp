#include "mdv_modbus_profile_ui.h"

#include "json.h"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

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

const mdvwb::json::Object& ObjectField(
    const mdvwb::json::Object& object,
    std::string_view name)
{
    return object.at(std::string(name)).AsObject();
}

const mdvwb::json::Array& ArrayField(
    const mdvwb::json::Object& object,
    std::string_view name)
{
    return object.at(std::string(name)).AsArray();
}

double NumberField(
    const mdvwb::json::Object& object,
    std::string_view name)
{
    const auto& value = object.at(std::string(name));
    return value.IsInteger()
        ? static_cast<double>(value.AsInteger())
        : value.AsNumber();
}

void TestProductionProfilePresentation()
{
    const auto jsonText = mdvwb::LoadModbusProfileUiCatalog(
        std::filesystem::path(MDVWB_SOURCE_DIR) / "profiles/modbus");
    const auto document = mdvwb::json::Parse(jsonText);
    const auto& root = document.AsObject();

    Require(root.at("schemaVersion").AsInteger() == 1,
            "UI schema version mismatch");

    const auto& profiles = ArrayField(root, "profiles");
    Require(profiles.size() == 1U, "production profile catalog size mismatch");

    const auto& profile = profiles.front().AsObject();
    Require(profile.at("id").AsString() == "vrf_add_controller",
            "production profile ID mismatch");
    Require(profile.at("name").AsString() == "VRF Add Controller",
            "production profile name mismatch");
    Require(profile.at("addressingType").AsString() == "fixed_slave_stride",
            "production addressing type mismatch");

    const auto& transport = ObjectField(profile, "transport");
    Require(transport.at("baudRate").AsInteger() == 9600,
            "production baud rate mismatch");
    Require(transport.at("dataBits").AsInteger() == 8,
            "production data bits mismatch");
    Require(transport.at("parity").AsString() == "none",
            "production parity mismatch");
    Require(transport.at("stopBits").AsInteger() == 1,
            "production stop bits mismatch");

    const auto& addresses = ObjectField(profile, "logicalAddresses");
    Require(addresses.at("minimum").AsInteger() == 1,
            "logical minimum mismatch");
    Require(addresses.at("maximum").AsInteger() == 63,
            "logical maximum mismatch");

    const auto& capabilities = ObjectField(profile, "capabilities");
    const auto& power = ObjectField(capabilities, "power");
    Require(power.at("supported").AsBoolean(), "Power not exposed");
    Require(power.at("readable").AsBoolean(), "Power read not exposed");
    Require(power.at("writable").AsBoolean(), "Power write not exposed");
    Require(power.at("type").AsString() == "boolean",
            "Power type mismatch");

    const auto& alarm = ObjectField(capabilities, "alarm");
    Require(alarm.at("supported").AsBoolean(), "Alarm not exposed");
    Require(alarm.at("readable").AsBoolean(), "Alarm read not exposed");
    Require(!alarm.at("writable").AsBoolean(),
            "read-only Alarm was exposed writable");

    for (const std::string name : {
             "mode", "fanSpeed", "setTemperature", "roomTemperature",
             "blinds", "blocked"}) {
        const auto& capability = ObjectField(capabilities, name);
        Require(!capability.at("supported").AsBoolean(),
                "disabled capability was exposed as supported: " + name);
    }

    Require(ArrayField(root, "issues").empty(),
            "valid production directory reported profile issues");
}

void TestEnumAndNumericUiMetadata()
{
    mdv::modbus::ModbusProfile profile;
    profile.id = "ui_rich_profile";
    profile.name = "UI Rich Profile";
    profile.transport = {
        .baudRate = 19200,
        .dataBits = 8,
        .parity = mdv::SerialParity::Even,
        .stopBits = 1,
    };
    profile.addressing = mdv::modbus::DirectSlaveAddressing{
        .logicalMin = 2,
        .logicalMax = 5,
        .registerOffset = 0,
    };
    profile.capabilities.mode = true;
    profile.capabilities.setTemperature = true;

    mdv::modbus::PointDefinition mode;
    mode.type = mdv::modbus::PointType::Enum;
    mode.read = mdv::modbus::RegisterLocation{};
    mode.write = mdv::modbus::RegisterLocation{};
    mode.enumMappings.read.emplace(0U, "off");
    mode.enumMappings.read.emplace(1U, "cool");
    mode.enumMappings.read.emplace(2U, "heat");
    mode.enumMappings.write.emplace("cool", 1U);
    mode.enumMappings.write.emplace("heat", 2U);
    profile.points.emplace("mode", mode);

    mdv::modbus::PointDefinition setTemperature;
    setTemperature.type = mdv::modbus::PointType::Number;
    setTemperature.read = mdv::modbus::RegisterLocation{};
    setTemperature.write = mdv::modbus::RegisterLocation{};
    setTemperature.limits = mdv::modbus::NumericLimits{
        .minimum = 16.0,
        .maximum = 30.0,
        .step = 0.5,
    };
    profile.points.emplace("setTemperature", setTemperature);

    mdv::modbus::ProfileCatalog catalog;
    catalog.profiles.emplace(profile.id, profile);
    const auto document = mdvwb::json::Parse(
        mdvwb::SerializeModbusProfileUiCatalog(catalog));
    const auto& root = document.AsObject();
    const auto& uiProfile = ArrayField(root, "profiles").front().AsObject();

    const auto& addresses = ObjectField(uiProfile, "logicalAddresses");
    Require(addresses.at("minimum").AsInteger() == 2,
            "synthetic logical minimum mismatch");
    Require(addresses.at("maximum").AsInteger() == 5,
            "synthetic logical maximum mismatch");

    const auto& capabilities = ObjectField(uiProfile, "capabilities");
    const auto& modeUi = ObjectField(capabilities, "mode");
    const auto& values = ArrayField(modeUi, "values");
    Require(values.size() == 3U, "enum semantic value count mismatch");

    const auto& cool = values.at(0).AsObject();
    Require(cool.at("value").AsString() == "cool",
            "enum values are not deterministic");
    Require(cool.at("readable").AsBoolean() &&
                cool.at("writable").AsBoolean(),
            "read/write enum metadata mismatch");

    const auto& off = values.at(2).AsObject();
    Require(off.at("value").AsString() == "off",
            "read-only enum option ordering mismatch");
    Require(off.at("readable").AsBoolean(),
            "read-only enum option not readable");
    Require(!off.at("writable").AsBoolean(),
            "read-only enum option exposed writable");

    const auto& setTemperatureUi =
        ObjectField(capabilities, "setTemperature");
    Require(NumberField(setTemperatureUi, "minimum") == 16.0,
            "numeric minimum mismatch");
    Require(NumberField(setTemperatureUi, "maximum") == 30.0,
            "numeric maximum mismatch");
    Require(NumberField(setTemperatureUi, "step") == 0.5,
            "numeric step mismatch");
}

void TestIncompatibleProfileIsNotPublished()
{
    mdv::modbus::ModbusProfile profile;
    profile.id = "write_only_power";
    profile.name = "Write-only Power";
    profile.capabilities.power = true;

    mdv::modbus::PointDefinition power;
    power.type = mdv::modbus::PointType::Boolean;
    power.write = mdv::modbus::RegisterLocation{};
    profile.points.emplace("power", power);

    mdv::modbus::ProfileCatalog catalog;
    catalog.profiles.emplace(profile.id, profile);

    const auto document = mdvwb::json::Parse(
        mdvwb::SerializeModbusProfileUiCatalog(catalog));
    const auto& root = document.AsObject();

    Require(ArrayField(root, "profiles").empty(),
            "runtime-incompatible profile was published to the UI");

    const auto& issues = ArrayField(root, "issues");
    Require(issues.size() == 1U,
            "runtime-incompatible profile did not produce one issue");

    const auto& issue = issues.front().AsObject();
    Require(issue.at("file").AsString() == "write_only_power.json",
            "runtime compatibility issue has the wrong profile identifier");
    Require(
        issue.at("message").AsString().find("without a read location") !=
            std::string::npos,
        "runtime compatibility issue does not explain the rejection");
}

void TestIssuePathIsSanitizedAndSorted()
{
    mdv::modbus::ProfileCatalog catalog;
    catalog.issues.push_back({"/secret/z-invalid.json", "z failure"});
    catalog.issues.push_back({"C:/private/a-invalid.json", "a failure"});

    const auto document = mdvwb::json::Parse(
        mdvwb::SerializeModbusProfileUiCatalog(catalog));
    const auto& root = document.AsObject();
    const auto& issues = ArrayField(root, "issues");
    Require(issues.size() == 2U, "profile issue count mismatch");

    const auto& first = issues.front().AsObject();
    Require(first.at("file").AsString() == "a-invalid.json",
            "profile issues are not deterministically sorted");
    Require(first.at("file").AsString().find("private") == std::string::npos,
            "profile issue leaked a server directory");
}

void TestJsonEscaping()
{
    mdv::modbus::ModbusProfile profile;
    profile.id = "escape_profile";
    profile.name = "Name \"with\" slash\\and\nline";
    profile.capabilities.power = true;

    mdv::modbus::PointDefinition power;
    power.type = mdv::modbus::PointType::Boolean;
    power.read = mdv::modbus::RegisterLocation{};
    profile.points.emplace("power", power);

    mdv::modbus::ProfileCatalog catalog;
    catalog.profiles.emplace(profile.id, profile);

    const auto document = mdvwb::json::Parse(
        mdvwb::SerializeModbusProfileUiCatalog(catalog));
    const auto& root = document.AsObject();
    const auto& restored = ArrayField(root, "profiles").front().AsObject();
    Require(restored.at("name").AsString() == profile.name,
            "profile name JSON escaping mismatch");
}

} // namespace

int main()
{
    try {
        TestProductionProfilePresentation();
        TestEnumAndNumericUiMetadata();
        TestIncompatibleProfileIsNotPublished();
        TestIssuePathIsSanitizedAndSorted();
        TestJsonEscaping();

        std::cout << "MDVWB Modbus profile UI catalog tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "MDVWB Modbus profile UI catalog tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
