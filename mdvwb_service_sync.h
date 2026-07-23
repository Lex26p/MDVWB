#pragma once

#include "mdv_buses_config.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace mdvwb {

struct ServiceSyncPaths {
    std::filesystem::path defaultDirectory = "/etc/default";
    std::filesystem::path environmentTemplate = "/usr/local/lib/mdvwb/mdvwb.env";
    std::string systemctlProgram = "systemctl";
};

enum class ServiceActionType {
    WriteConfig,
    RemoveConfig,
    EnableAndStart,
    EnableAndRestart,
    DisableAndStop,
    EnsureEnabledAndStarted,
};

struct ServiceAction {
    ServiceActionType type = ServiceActionType::WriteConfig;
    int busId = 0;
    std::filesystem::path configPath;
    std::string configContent;
};

struct ServiceSyncPlan {
    std::vector<ServiceAction> actions;
};

class CommandRunner {
public:
    virtual ~CommandRunner() = default;
    virtual int Run(const std::vector<std::string>& arguments) = 0;
};

class NativeCommandRunner final : public CommandRunner {
public:
    int Run(const std::vector<std::string>& arguments) override;
};

enum class BusServiceCommand {
    Start,
    Stop,
    Restart,
};

struct BusServiceStatus {
    bool active = false;
    bool enabled = false;
};

[[nodiscard]] std::string BusServiceName(int busId);
void ExecuteBusServiceCommand(
    int busId,
    BusServiceCommand command,
    const ServiceSyncPaths& paths,
    CommandRunner& runner);
[[nodiscard]] BusServiceStatus QueryBusServiceStatus(
    int busId,
    const ServiceSyncPaths& paths,
    CommandRunner& runner);

void WriteTextFileAtomically(const std::filesystem::path& path, std::string_view content);

ServiceSyncPaths ServiceSyncPathsFromEnvironment();
ServiceSyncPlan BuildServiceSyncPlan(const BusesConfig& config, const ServiceSyncPaths& paths);
void PrintServiceSyncPlan(const ServiceSyncPlan& plan, std::ostream& output);
void ApplyServiceSyncPlan(
    const ServiceSyncPlan& plan,
    const ServiceSyncPaths& paths,
    CommandRunner& runner);

}  // namespace mdvwb
