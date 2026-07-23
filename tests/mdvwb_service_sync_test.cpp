#include "mdvwb_service_sync.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-sync-test-" + std::to_string(token));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

class RecordingRunner final : public mdvwb::CommandRunner {
public:
    int Run(const std::vector<std::string>& arguments) override {
        commands.push_back(arguments);
        return returnCode;
    }

    int returnCode = 0;
    std::vector<std::vector<std::string>> commands;
};

void WriteTemplate(const std::filesystem::path& path) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output
        << "MDVWB_ADDRESSES=\"1\"\n"
        << "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        << "MDVWB_BUS=\"1\"\n"
        << "MDVWB_MASTER_ID=\"0\"\n"
        << "MDVWB_PERIOD_MS=\"150\"\n";
}

mdvwb::BusesConfig InitialConfig() {
    return mdvwb::ParseBusesConfig(R"json({
      "version": 1,
      "buses": [
        {"id": 1, "enabled": true, "port": "/dev/ttyRS485-1", "addresses": [3,1,2]},
        {"id": 2, "enabled": false, "port": "/dev/serial/by-id/mdv-bus-2", "addresses": []}
      ]
    })json");
}

bool ContainsCommand(
    const std::vector<std::vector<std::string>>& commands,
    const std::vector<std::string>& expected) {
    for (const auto& command : commands) {
        if (command == expected) {
            return true;
        }
    }
    return false;
}

void TestCreateAndApply() {
    TemporaryDirectory temporary;
    const auto defaults = temporary.Path() / "defaults";
    const auto environmentTemplate = temporary.Path() / "mdvwb.env";
    std::filesystem::create_directories(defaults);
    WriteTemplate(environmentTemplate);

    mdvwb::ServiceSyncPaths paths;
    paths.defaultDirectory = defaults;
    paths.environmentTemplate = environmentTemplate;
    paths.systemctlProgram = "fake-systemctl";

    const mdvwb::ServiceSyncPlan plan = mdvwb::BuildServiceSyncPlan(InitialConfig(), paths);
    Require(plan.actions.size() == 4, "new configuration must create two files and synchronize two services");

    std::ostringstream printed;
    mdvwb::PrintServiceSyncPlan(plan, printed);
    Require(printed.str().find("WRITE_CONFIG bus=1") != std::string::npos,
            "plan does not write bus 1 configuration");
    Require(printed.str().find("ENABLE_START bus=1") != std::string::npos,
            "plan does not start enabled bus 1");
    Require(printed.str().find("DISABLE_STOP bus=2") != std::string::npos,
            "plan does not stop disabled bus 2");

    RecordingRunner runner;
    mdvwb::ApplyServiceSyncPlan(plan, paths, runner);

    const std::string bus1 = ReadFile(defaults / "mdvwb-1");
    Require(bus1.find("# Managed by mdvwb-manager") == 0,
            "generated environment has no manager marker");
    Require(bus1.find("MDVWB_ADDRESSES=\"1,2,3\"") != std::string::npos,
            "generated addresses are wrong");
    Require(bus1.find("MDVWB_PORT=\"/dev/ttyRS485-1\"") != std::string::npos,
            "generated port is wrong");
    Require(bus1.find("MDVWB_BUS=\"1\"") != std::string::npos,
            "generated bus id is wrong");

    Require(ContainsCommand(
                runner.commands,
                {"fake-systemctl", "enable", "--now", "mdvwb@1.service"}),
            "enabled bus was not started");
    Require(ContainsCommand(
                runner.commands,
                {"fake-systemctl", "disable", "--now", "mdvwb@2.service"}),
            "disabled bus was not stopped");
}

void TestChangedAndRemovedBuses() {
    TemporaryDirectory temporary;
    const auto defaults = temporary.Path() / "defaults";
    const auto environmentTemplate = temporary.Path() / "mdvwb.env";
    std::filesystem::create_directories(defaults);
    WriteTemplate(environmentTemplate);

    mdvwb::ServiceSyncPaths paths;
    paths.defaultDirectory = defaults;
    paths.environmentTemplate = environmentTemplate;
    paths.systemctlProgram = "fake-systemctl";

    RecordingRunner initialRunner;
    mdvwb::ApplyServiceSyncPlan(
        mdvwb::BuildServiceSyncPlan(InitialConfig(), paths), paths, initialRunner);

    const mdvwb::BusesConfig changed = mdvwb::ParseBusesConfig(R"json({
      "version": 1,
      "buses": [
        {"id": 1, "enabled": true, "port": "/dev/ttyUSB7", "addresses": [5,6]}
      ]
    })json");

    const mdvwb::ServiceSyncPlan plan = mdvwb::BuildServiceSyncPlan(changed, paths);
    std::ostringstream printed;
    mdvwb::PrintServiceSyncPlan(plan, printed);
    Require(printed.str().find("ENABLE_RESTART bus=1") != std::string::npos,
            "changed enabled bus must restart");
    Require(printed.str().find("REMOVE_CONFIG bus=2") != std::string::npos,
            "removed managed bus must be deleted");

    RecordingRunner runner;
    mdvwb::ApplyServiceSyncPlan(plan, paths, runner);
    Require(!std::filesystem::exists(defaults / "mdvwb-2"),
            "removed bus configuration still exists");
    Require(ContainsCommand(
                runner.commands,
                {"fake-systemctl", "restart", "mdvwb@1.service"}),
            "changed bus was not restarted");
    Require(ContainsCommand(
                runner.commands,
                {"fake-systemctl", "disable", "--now", "mdvwb@2.service"}),
            "removed bus was not stopped");
}

void TestSystemctlFailure() {
    TemporaryDirectory temporary;
    const auto defaults = temporary.Path() / "defaults";
    const auto environmentTemplate = temporary.Path() / "mdvwb.env";
    std::filesystem::create_directories(defaults);
    WriteTemplate(environmentTemplate);

    mdvwb::ServiceSyncPaths paths;
    paths.defaultDirectory = defaults;
    paths.environmentTemplate = environmentTemplate;
    paths.systemctlProgram = "fake-systemctl";

    RecordingRunner runner;
    runner.returnCode = 5;
    try {
        mdvwb::ApplyServiceSyncPlan(
            mdvwb::BuildServiceSyncPlan(InitialConfig(), paths), paths, runner);
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find("systemctl command failed") != std::string::npos,
                "systemctl error has no useful message");
        return;
    }
    throw std::runtime_error("systemctl failure was ignored");
}

}  // namespace

int main() {
    try {
        TestCreateAndApply();
        TestChangedAndRemovedBuses();
        TestSystemctlFailure();
        std::cout << "MDVWB service synchronization tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB service synchronization tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
