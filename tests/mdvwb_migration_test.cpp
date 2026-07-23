#include "mdvwb_migration.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-migration-test-" + std::to_string(token));
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

class StatusRunner final : public mdvwb::CommandRunner {
public:
    int Run(const std::vector<std::string>& arguments) override {
        if (arguments.size() < 4U) {
            return 1;
        }
        const bool bus1 = arguments.back() == "mdvwb@1.service";
        if (arguments[1] == "is-active") {
            return bus1 ? 0 : 3;
        }
        if (arguments[1] == "is-enabled") {
            return bus1 ? 0 : 1;
        }
        return 0;
    }
};

void Write(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    if (!output) {
        throw std::runtime_error("cannot write test input");
    }
}

void TestMigration() {
    TemporaryDirectory temporary;
    mdvwb::ServiceSyncPaths paths;
    paths.defaultDirectory = temporary.Path();
    paths.systemctlProgram = "fake-systemctl";

    Write(temporary.Path() / "mdvwb-1",
        "MDVWB_ADDRESSES=\"3,1,2\"\n"
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_BUS=\"1\"\n");
    Write(temporary.Path() / "mdvwb-2",
        "MDVWB_ADDRESSES=\"18,5\"\n"
        "MDVWB_PORT=\"/dev/serial/by-id/mdv-bus-2\"\n"
        "MDVWB_BUS=\"2\"\n");
    Write(temporary.Path() / "unrelated", "VALUE=1\n");

    StatusRunner runner;
    const mdvwb::BusesConfig config = mdvwb::MigrateLegacyDefaults(paths, runner);
    Require(config.buses.size() == 2U, "wrong migrated bus count");
    Require(config.buses[0].id == 1 && config.buses[0].enabled,
            "bus 1 service state was not migrated");
    Require(config.buses[0].addresses == std::vector<int>({1, 2, 3}),
            "bus 1 addresses were not normalized");
    Require(config.buses[1].id == 2 && !config.buses[1].enabled,
            "bus 2 service state was not migrated");
    Require(config.buses[1].port == "/dev/serial/by-id/mdv-bus-2",
            "custom bus port was not migrated");
}

} // namespace

int main() {
    try {
        TestMigration();
        std::cout << "MDVWB legacy migration tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB legacy migration tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
