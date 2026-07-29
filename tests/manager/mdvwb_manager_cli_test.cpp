#include "mdvwb_manager_cli.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void SetEnvironment(std::string_view name, std::optional<std::string_view> value) {
#ifdef _WIN32
    const std::string text = value.has_value() ? std::string(*value) : std::string{};
    if (_putenv_s(std::string(name).c_str(), text.c_str()) != 0) {
        throw std::runtime_error("cannot update test environment");
    }
#else
    const int result = value.has_value()
        ? setenv(std::string(name).c_str(), std::string(*value).c_str(), 1)
        : unsetenv(std::string(name).c_str());
    if (result != 0) {
        throw std::runtime_error("cannot update test environment");
    }
#endif
}

class ScopedEnvironment final {
public:
    ScopedEnvironment(std::string name, std::string value)
        : name_(std::move(name)) {
        if (const char* current = std::getenv(name_.c_str()); current != nullptr) {
            previous_ = current;
        }
        SetEnvironment(name_, std::string_view(value));
    }

    ~ScopedEnvironment() {
        try {
            if (previous_.has_value()) {
                SetEnvironment(name_, std::string_view(*previous_));
            } else {
                SetEnvironment(name_, std::nullopt);
            }
        } catch (...) {
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static unsigned long long counter = 0;
        const auto token =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-manager-cli-test-" + std::to_string(token) + "-" +
             std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& Path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class TemporaryConfig final {
public:
    explicit TemporaryConfig(std::string_view content) {
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("mdvwb-manager-test-" + std::to_string(token) + ".json");
        std::ofstream output(path_, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create temporary configuration");
        }
        output << content;
        if (!output) {
            throw std::runtime_error("cannot write temporary configuration");
        }
    }

    ~TemporaryConfig() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

void Write(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create test file");
    }
    output << content;
    if (!output) {
        throw std::runtime_error("cannot write test file");
    }
}

struct CommandResult {
    int code = 0;
    std::string output;
    std::string errors;
};

CommandResult Run(std::vector<std::string> arguments) {
    std::ostringstream output;
    std::ostringstream errors;
    const int code = mdvwb::RunManagerCommand(arguments, output, errors);
    return {code, output.str(), errors.str()};
}

std::string ValidConfig() {
    return R"json({
      "version": 1,
      "buses": [
        {"id": 2, "enabled": false, "port": "/dev/ttyUSB0", "addresses": []},
        {"id": 1, "enabled": true, "port": "/dev/ttyRS485-1", "addresses": [3, 1, 2]}
      ]
    })json";
}

void TestValidate() {
    TemporaryConfig config(ValidConfig());
    const CommandResult result = Run({"validate", config.Path().string()});
    Require(result.code == 0, "validate returned an error");
    Require(result.output == "CONFIG_OK buses=2 enabled=1\n", "unexpected validate output");
    Require(result.errors.empty(), "validate wrote an error");
}

void TestShowCanonicalJson() {
    TemporaryConfig config(ValidConfig());
    const CommandResult result = Run({"show", config.Path().string()});
    Require(result.code == 0, "show returned an error");
    Require(result.output.find("\"id\": 1") < result.output.find("\"id\": 2"),
            "show did not sort buses");
    Require(result.output.find("\"addresses\": [1, 2, 3]") != std::string::npos,
            "show did not sort addresses");
}

void TestSummary() {
    TemporaryConfig config(ValidConfig());
    const CommandResult result = Run({"summary", config.Path().string()});
    Require(result.code == 0, "summary returned an error");
    Require(result.output.find("version=1\nbuses=2\nenabled=1\n") == 0,
            "summary header is wrong");
    Require(result.output.find(
        "bus=1 enabled=true port=/dev/ttyRS485-1 addresses=1,2,3\n") != std::string::npos,
        "enabled bus summary is wrong");
    Require(result.output.find(
        "bus=2 enabled=false port=/dev/ttyUSB0 addresses=\n") != std::string::npos,
        "disabled bus summary is wrong");
}

void TestMigrationNotFoundExitCode() {
    TemporaryDirectory defaults;
    const std::filesystem::path target = defaults.Path() / "buses.json";
    const ScopedEnvironment allow("MDVWB_ALLOW_UNPRIVILEGED_APPLY", "1");
    const ScopedEnvironment directory("MDVWB_DEFAULT_DIR", defaults.Path().string());

    const CommandResult result =
        Run({"migrate-defaults", target.string()});

    Require(result.code == 3, "missing legacy configuration did not return code 3");
    Require(result.output.empty(), "missing legacy configuration wrote normal output");
    Require(
        result.errors ==
            "MIGRATION_NOT_FOUND: no legacy MDVWB bus configurations were found\n",
        "missing legacy configuration has an unstable result");
    Require(!std::filesystem::exists(target), "missing migration created a target file");
}

void TestMalformedMigrationIsNotReportedAsMissing() {
    TemporaryDirectory defaults;
    const std::filesystem::path target = defaults.Path() / "buses.json";
    Write(
        defaults.Path() / "mdvwb-1",
        "MDVWB_PORT=\"/dev/ttyRS485-1\"\n"
        "MDVWB_BUS=\"1\"\n");

    const ScopedEnvironment allow("MDVWB_ALLOW_UNPRIVILEGED_APPLY", "1");
    const ScopedEnvironment directory("MDVWB_DEFAULT_DIR", defaults.Path().string());

    const CommandResult result =
        Run({"migrate-defaults", target.string()});

    Require(result.code == 2, "malformed legacy configuration did not return code 2");
    Require(
        result.errors.find("CONFIG_ERROR:") == 0,
        "malformed legacy configuration was reported as missing");
    Require(
        result.errors.find("MDVWB_ADDRESSES") != std::string::npos,
        "malformed migration lost its validation detail");
    Require(!std::filesystem::exists(target), "failed migration created a target file");
}

void TestErrors() {
    TemporaryConfig invalid(R"json({"version":1,"buses":[
      {"id":1,"enabled":true,"port":"tty0","addresses":[1]}
    ]})json");

    const CommandResult invalidResult = Run({"validate", invalid.Path().string()});
    Require(invalidResult.code == 2, "invalid configuration did not return code 2");
    Require(invalidResult.errors.find("CONFIG_ERROR:") == 0,
            "invalid configuration has no stable error prefix");

    const CommandResult unknown = Run({"remove", invalid.Path().string()});
    Require(unknown.code == 2, "unknown command did not return code 2");
    Require(unknown.errors.find("USAGE_ERROR:") == 0,
            "unknown command has no usage error prefix");

    const CommandResult help = Run({"help"});
    Require(help.output.find("mdvwb-manager plan") != std::string::npos,
            "help does not mention plan");
    Require(help.output.find("mdvwb-manager apply") != std::string::npos,
            "help does not mention apply");
    Require(help.output.find("mdvwb-manager mqtt") != std::string::npos,
            "help does not mention mqtt");
    Require(help.output.find("returns code 3") != std::string::npos,
            "help does not document the no-legacy result");
}

}  // namespace

int main() {
    try {
        TestValidate();
        TestShowCanonicalJson();
        TestSummary();
        TestMigrationNotFoundExitCode();
        TestMalformedMigrationIsNotReportedAsMissing();
        TestErrors();
        std::cout << "MDVWB manager CLI tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB manager CLI tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
