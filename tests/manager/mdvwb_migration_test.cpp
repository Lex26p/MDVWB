#include "mdvwb_migration.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
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
            ("mdvwb-migration-test-" + std::to_string(token) + "-" +
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

class StatusRunner final : public mdvwb::CommandRunner {
public:
    int Run(const std::vector<std::string>& arguments) override
    {
        ++calls;
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
        return 1;
    }

    int calls = 0;
};

void Write(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    if (!output) {
        throw std::runtime_error("cannot write test input");
    }
}

void ExpectConfigError(
    const std::function<void()>& action,
    std::string_view expected,
    std::string_view failureMessage)
{
    try {
        action();
    } catch (const mdvwb::BusesConfigError& error) {
        Require(
            std::string_view(error.what()).find(expected) !=
                std::string_view::npos,
            std::string(failureMessage) + ": wrong error: " + error.what());
        return;
    }
    throw std::runtime_error(std::string(failureMessage));
}

mdvwb::ServiceSyncPaths Paths(const TemporaryDirectory& temporary)
{
    mdvwb::ServiceSyncPaths paths;
    paths.defaultDirectory = temporary.Path();
    paths.systemctlProgram = "fake-systemctl";
    return paths;
}

void TestMigration()
{
    TemporaryDirectory temporary;
    Write(
        temporary.Path() / "mdvwb-1",
        "MDVWB_ADDRESSES=\"3,1,2\"\n"
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_BUS=\"1\"\n"
        "MDVWB_PERIOD_MS=\"150\"\n");
    Write(
        temporary.Path() / "mdvwb-2",
        "MDVWB_ADDRESSES=\"18,5\"\n"
        "MDVWB_PORT=\"/dev/serial/by-id/mdv-bus-2\"\n"
        "MDVWB_BUS=\"2\"\n");
    Write(temporary.Path() / "mdvwb-backup", "not a migration candidate\n");
    Write(temporary.Path() / "unrelated", "VALUE=1\n");

    StatusRunner runner;
    const mdvwb::BusesConfig config =
        mdvwb::MigrateLegacyDefaults(Paths(temporary), runner);
    Require(config.buses.size() == 2U, "wrong migrated bus count");
    Require(
        config.buses[0].id == 1 && config.buses[0].enabled,
        "bus 1 service state was not migrated");
    Require(
        config.buses[0].addresses == std::vector<int>({1, 2, 3}),
        "bus 1 addresses were not normalized");
    Require(
        config.buses[1].id == 2 && !config.buses[1].enabled,
        "bus 2 service state was not migrated");
    Require(
        config.buses[1].port == "/dev/serial/by-id/mdv-bus-2",
        "custom bus port was not migrated");
    Require(runner.calls == 4, "service state was not queried exactly once per field");
}

void TestMissingRequiredAssignmentFails()
{
    TemporaryDirectory temporary;
    Write(
        temporary.Path() / "mdvwb-1",
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_BUS=\"1\"\n");
    StatusRunner runner;
    ExpectConfigError(
        [&] { static_cast<void>(mdvwb::MigrateLegacyDefaults(Paths(temporary), runner)); },
        "missing required assignment MDVWB_ADDRESSES",
        "incomplete legacy file was accepted");
    Require(runner.calls == 0, "migration queried services before validating all files");
}

void TestFilenameAndDeclaredBusMismatchFails()
{
    TemporaryDirectory temporary;
    Write(
        temporary.Path() / "mdvwb-1",
        "MDVWB_ADDRESSES=\"1\"\n"
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_BUS=\"2\"\n");
    StatusRunner runner;
    ExpectConfigError(
        [&] { static_cast<void>(mdvwb::MigrateLegacyDefaults(Paths(temporary), runner)); },
        "ambiguous bus id",
        "filename and MDVWB_BUS mismatch was accepted");
    Require(runner.calls == 0, "ambiguous migration queried service state");
}

void TestDuplicateBusSourcesFail()
{
    TemporaryDirectory temporary;
    Write(
        temporary.Path() / "mdvwb",
        "MDVWB_ADDRESSES=\"1\"\n"
        "MDVWB_PORT=\"/dev/ttyRS485-old\"\n"
        "MDVWB_BUS=\"1\"\n");
    Write(
        temporary.Path() / "mdvwb-1",
        "MDVWB_ADDRESSES=\"2\"\n"
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_BUS=\"1\"\n");
    StatusRunner runner;
    ExpectConfigError(
        [&] { static_cast<void>(mdvwb::MigrateLegacyDefaults(Paths(temporary), runner)); },
        "defined by both",
        "duplicate legacy bus sources were accepted");
    Require(runner.calls == 0, "duplicate migration queried service state");
}

void TestDuplicateAssignmentFails()
{
    TemporaryDirectory temporary;
    Write(
        temporary.Path() / "mdvwb-1",
        "MDVWB_ADDRESSES=\"1\"\n"
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_PORT=\"/dev/ttyRS485-other\"\n"
        "MDVWB_BUS=\"1\"\n");
    StatusRunner runner;
    ExpectConfigError(
        [&] { static_cast<void>(mdvwb::MigrateLegacyDefaults(Paths(temporary), runner)); },
        "duplicate assignment for MDVWB_PORT",
        "duplicate assignment used last-value-wins semantics");
}

void TestMalformedLineFails()
{
    TemporaryDirectory temporary;
    Write(
        temporary.Path() / "mdvwb-1",
        "MDVWB_ADDRESSES=\"1\"\n"
        "this is not an assignment\n"
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_BUS=\"1\"\n");
    StatusRunner runner;
    ExpectConfigError(
        [&] { static_cast<void>(mdvwb::MigrateLegacyDefaults(Paths(temporary), runner)); },
        "expected NAME=VALUE assignment",
        "malformed line was ignored");
}

void TestUnterminatedQuoteFails()
{
    TemporaryDirectory temporary;
    Write(
        temporary.Path() / "mdvwb-1",
        "MDVWB_ADDRESSES=\"1\"\n"
        "MDVWB_PORT=\"/dev/ttyRS485-1\n"
        "MDVWB_BUS=\"1\"\n");
    StatusRunner runner;
    ExpectConfigError(
        [&] { static_cast<void>(mdvwb::MigrateLegacyDefaults(Paths(temporary), runner)); },
        "unterminated quoted value for MDVWB_PORT",
        "unterminated quote was accepted");
}


void TestInvalidPortFailsBeforeServiceQuery()
{
    TemporaryDirectory temporary;
    Write(
        temporary.Path() / "mdvwb-1",
        "MDVWB_ADDRESSES=\"1\"\n"
        "MDVWB_PORT=\"ttyRS485-1\"\n"
        "MDVWB_BUS=\"1\"\n");
    StatusRunner runner;
    ExpectConfigError(
        [&] { static_cast<void>(mdvwb::MigrateLegacyDefaults(Paths(temporary), runner)); },
        "port",
        "invalid legacy port was accepted");
    Require(runner.calls == 0, "invalid port was checked after querying service state");
}

void TestNonCanonicalFilenameFails()
{
    TemporaryDirectory temporary;
    Write(
        temporary.Path() / "mdvwb-01",
        "MDVWB_ADDRESSES=\"1\"\n"
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_BUS=\"1\"\n");
    StatusRunner runner;
    ExpectConfigError(
        [&] { static_cast<void>(mdvwb::MigrateLegacyDefaults(Paths(temporary), runner)); },
        "without leading zeros",
        "non-canonical filename alias was accepted");
}

}  // namespace

int main()
{
    try {
        TestMigration();
        TestMissingRequiredAssignmentFails();
        TestFilenameAndDeclaredBusMismatchFails();
        TestDuplicateBusSourcesFail();
        TestDuplicateAssignmentFails();
        TestMalformedLineFails();
        TestUnterminatedQuoteFails();
        TestInvalidPortFailsBeforeServiceQuery();
        TestNonCanonicalFilenameFails();
        std::cout << "MDVWB strict legacy migration tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB strict legacy migration tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
