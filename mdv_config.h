#pragma once

#include "mdv_mosquitto.h"
#include "mdv_serial.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mdv {

enum class ManualControlKind {
    Power,
    Mode,
    Speed,
    SetTemperature,
    Blinds,
    Block,
};

struct ManualControlCommand {
    ManualControlKind kind = ManualControlKind::Power;
    int value = 0;
};

struct ApplicationConfig {
    std::vector<std::uint8_t> addresses;
    std::string serialPort;
    int busNumber = 1;
    std::uint8_t masterId = 0;
    TimingSettings timing{};
    MqttConnectionOptions mqtt{};
    bool publishPollAddress = false;
    bool readOnly = false;
    bool discover = false;
    std::optional<ManualControlCommand> manualControl;
};

enum class CommandLineAction {
    Run,
    SelfTest,
    Help,
    Version,
};

struct CommandLineResult {
    CommandLineAction action = CommandLineAction::Run;
    ApplicationConfig config{};
};

[[nodiscard]] CommandLineResult ParseCommandLine(int argc, char* argv[]);
[[nodiscard]] std::string BuildHelpText(std::string_view executableName);
[[nodiscard]] std::string FormatAddressList(
    const std::vector<std::uint8_t>& addresses);

} // namespace mdv
