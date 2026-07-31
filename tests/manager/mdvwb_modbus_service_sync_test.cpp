#include "mdvwb_service_sync.h"

#include <chrono>
#include <filesystem>
#include <fstream>
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

class TemporaryDirectory final {
public:
    TemporaryDirectory()
    {
        const auto token =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-modbus-sync-test-" + std::to_string(token));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& Path() const noexcept
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void WriteText(
    const std::filesystem::path& path,
    std::string_view content)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create test file");
    }
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write test file");
    }
}

std::string EscapeShellEnvironmentValue(std::string_view value)
{
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '\\' || character == '"' ||
            character == '$' || character == '`') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

void WriteTemplate(const std::filesystem::path& path)
{
    WriteText(
        path,
        "MDVWB_ADDRESSES=\"1\"\n"
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_BUS=\"1\"\n"
        "MDVWB_PROTOCOL=\"mdv\"\n"
        "MDVWB_MODBUS_PROFILE=\"stale\"\n"
        "MDVWB_MODBUS_PROFILE_DIR=\"/stale\"\n"
        "MDVWB_MODBUS_BAUD_RATE=\"19200\"\n"
        "MDVWB_MODBUS_DATA_BITS=\"7\"\n"
        "MDVWB_MODBUS_PARITY=\"odd\"\n"
        "MDVWB_MODBUS_STOP_BITS=\"2\"\n");
}

mdvwb::ServiceSyncPaths MakePaths(const TemporaryDirectory& temporary)
{
    mdvwb::ServiceSyncPaths paths;
    paths.defaultDirectory = temporary.Path() / "defaults";
    paths.environmentTemplate = temporary.Path() / "mdvwb.env";
    paths.modbusProfileDirectory = temporary.Path() / "profiles";
    paths.systemctlProgram = "fake-systemctl";
    std::filesystem::create_directories(paths.defaultDirectory);
    WriteTemplate(paths.environmentTemplate);
    return paths;
}

void InstallProductionProfile(const mdvwb::ServiceSyncPaths& paths)
{
    std::filesystem::create_directories(paths.modbusProfileDirectory);
    const auto source =
        std::filesystem::path(MDVWB_SOURCE_DIR) /
        "profiles/modbus/vrf_add_controller.json";
    std::filesystem::copy_file(
        source,
        paths.modbusProfileDirectory / "vrf_add_controller.json",
        std::filesystem::copy_options::overwrite_existing);
}

std::string WriteConfigContent(const mdvwb::ServiceSyncPlan& plan)
{
    for (const auto& action : plan.actions) {
        if (action.type == mdvwb::ServiceActionType::WriteConfig) {
            return action.configContent;
        }
    }
    throw std::runtime_error("service plan did not contain WriteConfig");
}

mdvwb::BusesConfig ModbusConfig(std::string_view profileId)
{
    return mdvwb::ParseBusesConfig(
        "{\"version\":1,\"buses\":[{"
        "\"id\":2,"
        "\"enabled\":true,"
        "\"protocol\":\"modbus_rtu\","
        "\"port\":\"/dev/ttyRS485-2\","
        "\"modbus\":{"
        "\"profileId\":\"" + std::string(profileId) + "\","
        "\"baudRate\":9600,"
        "\"dataBits\":8,"
        "\"parity\":\"none\","
        "\"stopBits\":1},"
        "\"addresses\":[1,2,63]}]}"
    );
}

void TestModbusRuntimeEnvironment()
{
    TemporaryDirectory temporary;
    auto paths = MakePaths(temporary);
    InstallProductionProfile(paths);

    const auto plan =
        mdvwb::BuildServiceSyncPlan(ModbusConfig("vrf_add_controller"), paths);
    const std::string content = WriteConfigContent(plan);

    Require(
        content.find("MDVWB_PROTOCOL=\"modbus_rtu\"") != std::string::npos,
        "runtime environment omitted Modbus protocol");
    Require(
        content.find("MDVWB_MODBUS_PROFILE=\"vrf_add_controller\"") !=
            std::string::npos,
        "runtime environment omitted profile ID");
    Require(
        content.find(
            "MDVWB_MODBUS_PROFILE_DIR=\"" +
            EscapeShellEnvironmentValue(paths.modbusProfileDirectory.string()) +
            "\"") != std::string::npos,
        "runtime environment omitted profile directory");
    Require(
        content.find("MDVWB_MODBUS_BAUD_RATE=\"9600\"") != std::string::npos,
        "runtime environment omitted baud rate");
    Require(
        content.find("MDVWB_MODBUS_DATA_BITS=\"8\"") != std::string::npos,
        "runtime environment omitted data bits");
    Require(
        content.find("MDVWB_MODBUS_PARITY=\"none\"") != std::string::npos,
        "runtime environment omitted parity");
    Require(
        content.find("MDVWB_MODBUS_STOP_BITS=\"1\"") != std::string::npos,
        "runtime environment omitted stop bits");
    Require(
        content.find("MDVWB_ADDRESSES=\"1,2,63\"") != std::string::npos,
        "runtime environment omitted logical addresses");
}

void TestUnknownProfileBlocksServicePlan()
{
    TemporaryDirectory temporary;
    auto paths = MakePaths(temporary);
    InstallProductionProfile(paths);

    try {
        static_cast<void>(
            mdvwb::BuildServiceSyncPlan(ModbusConfig("missing_profile"), paths));
    }
    catch (const mdvwb::BusesConfigError& error) {
        Require(
            std::string_view(error.what()).find("unknown Modbus profile") !=
                std::string_view::npos,
            "unknown profile produced the wrong error");
        return;
    }

    throw std::runtime_error("service plan accepted an unknown Modbus profile");
}

void TestLegacyMdvDoesNotNeedProfileDirectory()
{
    TemporaryDirectory temporary;
    auto paths = MakePaths(temporary);
    paths.modbusProfileDirectory = temporary.Path() / "does-not-exist";

    const auto config = mdvwb::ParseBusesConfig(R"json(
    {
      "version": 1,
      "buses": [
        {
          "id": 1,
          "enabled": true,
          "port": "/dev/ttyRS485-1",
          "addresses": [0,1,2]
        }
      ]
    })json");

    const auto plan = mdvwb::BuildServiceSyncPlan(config, paths);
    const std::string content = WriteConfigContent(plan);

    Require(
        content.find("MDVWB_PROTOCOL=\"mdv\"") != std::string::npos,
        "legacy MDV bus lost its default protocol");
    Require(
        content.find("MDVWB_MODBUS_PROFILE=\"\"") != std::string::npos,
        "MDV bus retained stale Modbus profile data");
    Require(
        content.find("MDVWB_MODBUS_PROFILE_DIR=\"\"") != std::string::npos,
        "MDV bus retained stale Modbus profile directory");
}

} // namespace

int main()
{
    try {
        TestModbusRuntimeEnvironment();
        TestUnknownProfileBlocksServicePlan();
        TestLegacyMdvDoesNotNeedProfileDirectory();

        std::cout << "MDVWB Modbus service synchronization tests: OK\n";
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr
            << "MDVWB Modbus service synchronization tests: FAILED: "
            << error.what() << '\n';
        return 1;
    }
}
