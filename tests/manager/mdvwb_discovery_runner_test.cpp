#include "mdvwb_discovery_runner.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void RequireThrows(Function&& function, std::string_view message) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

#ifndef _WIN32
class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto token = std::chrono::steady_clock::now()
            .time_since_epoch()
            .count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-discovery-runner-" + std::to_string(token));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& Path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void WriteExecutable(
    const std::filesystem::path& path,
    std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write discovery test executable");
    }
    output.close();
    if (chmod(path.c_str(), 0755) != 0) {
        throw std::runtime_error("cannot mark discovery test executable");
    }
}

void TestNativeRunnerTerminatesTimedOutProcess() {
    TemporaryDirectory temporary;
    const std::filesystem::path script =
        temporary.Path() / "timeout.sh";
    WriteExecutable(
        script,
        "#!/bin/sh\n"
        "trap '' TERM\n"
        "while :; do :; done\n");

    mdvwb::NativeDiscoveryRunner runner(
        script.string(),
        std::chrono::milliseconds(100),
        4096U);
    const auto started = std::chrono::steady_clock::now();
    const mdvwb::DiscoveryExecutionResult result =
        runner.Run("/dev/null", 0, 150, 130);
    const auto elapsed =
        std::chrono::steady_clock::now() - started;

    Require(!result.success, "timed-out discovery was successful");
    Require(
        result.message.find("timed out") != std::string::npos,
        "timeout result does not explain the failure");
    Require(
        elapsed < std::chrono::seconds(3),
        "timed-out child process was not terminated promptly");
}

void TestNativeRunnerLimitsCapturedOutput() {
    TemporaryDirectory temporary;
    const std::filesystem::path script =
        temporary.Path() / "output-limit.sh";
    WriteExecutable(
        script,
        "#!/bin/sh\n"
        "while :; do\n"
        "  printf '0123456789abcdef0123456789abcdef'\n"
        "done\n");

    constexpr std::size_t MaximumOutput = 1024U;
    mdvwb::NativeDiscoveryRunner runner(
        script.string(),
        std::chrono::seconds(5),
        MaximumOutput);
    const mdvwb::DiscoveryExecutionResult result =
        runner.Run("/dev/null", 0, 150, 130);

    Require(!result.success, "oversized discovery output was accepted");
    Require(
        result.message.find("output exceeded") != std::string::npos,
        "output-limit result does not explain the failure");
    Require(
        result.output.size() == MaximumOutput,
        "captured discovery output was not bounded");
}
#endif

}  // namespace

int main() {
    try {
        const auto addresses = mdvwb::ParseDiscoveryAddresses(
            "Discovery pass 1/3 started.\n"
            "Found MDV addresses: 1,3,18\n"
            "FOUND_ADDRESSES=1,3,18\n");
        Require(
            addresses == std::vector<int>({1, 3, 18}),
            "valid addresses were not parsed");

        const auto empty = mdvwb::ParseDiscoveryAddresses(
            "No MDV fan coils found.\nFOUND_ADDRESSES=\n");
        Require(
            empty.empty(),
            "empty discovery result was not parsed");

        const auto last = mdvwb::ParseDiscoveryAddresses(
            "FOUND_ADDRESSES=1\nFOUND_ADDRESSES=2,4\r\n");
        Require(
            last == std::vector<int>({2, 4}),
            "last machine-readable result was not used");

        RequireThrows(
            [] {
                static_cast<void>(
                    mdvwb::ParseDiscoveryAddresses("no result\n"));
            },
            "missing result marker was accepted");
        RequireThrows(
            [] {
                static_cast<void>(
                    mdvwb::ParseDiscoveryAddresses(
                        "FOUND_ADDRESSES=1,64\n"));
            },
            "out-of-range address was accepted");
        RequireThrows(
            [] {
                static_cast<void>(
                    mdvwb::ParseDiscoveryAddresses(
                        "FOUND_ADDRESSES=1,1\n"));
            },
            "duplicate address was accepted");
        RequireThrows(
            [] {
                static_cast<void>(
                    mdvwb::ParseDiscoveryAddresses(
                        "FOUND_ADDRESSES=1,,2\n"));
            },
            "empty address was accepted");

        RequireThrows(
            [] {
                mdvwb::NativeDiscoveryRunner runner(
                    "program",
                    std::chrono::milliseconds(0));
            },
            "zero discovery timeout was accepted");
        RequireThrows(
            [] {
                mdvwb::NativeDiscoveryRunner runner(
                    "program",
                    std::chrono::seconds(1),
                    0U);
            },
            "zero discovery output limit was accepted");

#ifndef _WIN32
        TestNativeRunnerTerminatesTimedOutProcess();
        TestNativeRunnerLimitsCapturedOutput();
#endif

        std::cout << "MDVWB discovery runner tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB discovery runner tests: FAILED: "
                  << error.what() << '\n';
        return 1;
    }
}
