#include "mdvwb_manager_cli.h"

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
}

}  // namespace

int main() {
    try {
        TestValidate();
        TestShowCanonicalJson();
        TestSummary();
        TestErrors();
        std::cout << "MDVWB manager CLI tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB manager CLI tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
