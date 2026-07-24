#include "mdvwb_manager_cli.h"

#include "mdv_buses_config.h"
#include "mdvwb_service_sync.h"
#include "mdvwb_manager_mqtt.h"
#include "mdvwb_migration.h"

#include <cstdlib>
#include <filesystem>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace mdvwb {
namespace {

constexpr std::string_view DefaultConfigPath = "/etc/mdvwb/buses.json";

std::filesystem::path ResolveConfigPath(const std::vector<std::string>& arguments) {
    if (arguments.size() >= 2U) {
        return arguments[1];
    }

    if (const char* environmentPath = std::getenv("MDVWB_BUSES_CONFIG");
        environmentPath != nullptr && environmentPath[0] != '\0') {
        return environmentPath;
    }

    return std::filesystem::path(DefaultConfigPath);
}

std::string JoinAddresses(const std::vector<int>& addresses) {
    std::ostringstream result;
    for (std::size_t index = 0; index < addresses.size(); ++index) {
        if (index != 0U) {
            result << ',';
        }
        result << addresses[index];
    }
    return result.str();
}

void PrintHelp(std::ostream& output) {
    output
        << "MDVWB configuration manager\n"
        << "\n"
        << "Usage:\n"
        << "  mdvwb-manager validate [buses.json]\n"
        << "  mdvwb-manager show [buses.json]\n"
        << "  mdvwb-manager summary [buses.json]\n"
        << "  mdvwb-manager plan [buses.json]\n"
        << "  mdvwb-manager apply [buses.json]\n"
        << "  mdvwb-manager mqtt [buses.json]\n"
        << "  mdvwb-manager migrate-defaults [buses.json]\n"
        << "\n"
        << "plan prints the systemd and /etc/default changes without applying them.\n"
        << "apply writes managed /etc/default/mdvwb-N files and synchronizes\n"
        << "mdvwb@N.service instances. apply must be run as root.\n"
        << "mqtt runs the long-lived MQTT configuration endpoint and must be run as root.\n"
        << "migrate-defaults converts existing /etc/default/mdvwb-N files to buses.json.\n"
        << "\n"
        << "When the path is omitted, MDVWB_BUSES_CONFIG is used if set;\n"
        << "otherwise /etc/mdvwb/buses.json is used.\n";
}

int PrintValidation(const BusesConfig& config, std::ostream& output) {
    std::size_t enabledCount = 0;
    for (const BusConfig& bus : config.buses) {
        if (bus.enabled) {
            ++enabledCount;
        }
    }

    output << "CONFIG_OK buses=" << config.buses.size()
           << " enabled=" << enabledCount << '\n';
    return 0;
}

int PrintSummary(const BusesConfig& config, std::ostream& output) {
    std::size_t enabledCount = 0;
    for (const BusConfig& bus : config.buses) {
        if (bus.enabled) {
            ++enabledCount;
        }
    }

    output << "version=" << config.version << '\n'
           << "buses=" << config.buses.size() << '\n'
           << "enabled=" << enabledCount << '\n';

    for (const BusConfig& bus : config.buses) {
        output << "bus=" << bus.id
               << " enabled=" << (bus.enabled ? "true" : "false")
               << " port=" << bus.port
               << " addresses=" << JoinAddresses(bus.addresses)
               << '\n';
    }
    return 0;
}

bool IsApplyAllowed() {
    if (const char* overrideValue = std::getenv("MDVWB_ALLOW_UNPRIVILEGED_APPLY");
        overrideValue != nullptr && std::string_view(overrideValue) == "1") {
        return true;
    }
#ifdef _WIN32
    return false;
#else
    return geteuid() == 0;
#endif
}

}  // namespace

int RunManagerCommand(
    const std::vector<std::string>& arguments,
    std::ostream& output,
    std::ostream& errors) {
    if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h" ||
        arguments[0] == "help") {
        PrintHelp(output);
        return 0;
    }

    if (arguments.size() > 2U) {
        errors << "USAGE_ERROR: too many arguments\n";
        PrintHelp(errors);
        return 2;
    }

    const std::string& command = arguments[0];
    if (command != "validate" && command != "show" && command != "summary" &&
        command != "plan" && command != "apply" && command != "mqtt" &&
        command != "migrate-defaults") {
        errors << "USAGE_ERROR: unknown command '" << command << "'\n";
        PrintHelp(errors);
        return 2;
    }

    try {
        const std::filesystem::path configPath = ResolveConfigPath(arguments);

        if (command == "mqtt") {
            if (!IsApplyAllowed()) {
                errors << "MANAGER_ERROR: mqtt must be run as root\n";
                return 1;
            }
            return RunManagerMqttDaemon(configPath, output, errors);
        }

        if (command == "migrate-defaults") {
            if (!IsApplyAllowed()) {
                errors << "MANAGER_ERROR: migrate-defaults must be run as root\n";
                return 1;
            }
            const ServiceSyncPaths paths = ServiceSyncPathsFromEnvironment();
            NativeCommandRunner runner;
            const BusesConfig migrated = MigrateLegacyDefaults(paths, runner);
            WriteTextFileAtomically(configPath, SerializeBusesConfig(migrated));
            output << "MIGRATED buses=" << migrated.buses.size()
                   << " path=" << configPath.string() << '\n';
            return 0;
        }

        const BusesConfig config = LoadBusesConfig(configPath);

        if (command == "validate") {
            return PrintValidation(config, output);
        }
        if (command == "show") {
            output << SerializeBusesConfig(config);
            return 0;
        }
        if (command == "summary") {
            return PrintSummary(config, output);
        }

        const ServiceSyncPaths paths = ServiceSyncPathsFromEnvironment();
        const ServiceSyncPlan plan = BuildServiceSyncPlan(config, paths);
        PrintServiceSyncPlan(plan, output);
        if (command == "plan") {
            return 0;
        }

        if (!IsApplyAllowed()) {
            errors << "MANAGER_ERROR: apply must be run as root\n";
            return 1;
        }
        NativeCommandRunner runner;
        ApplyServiceSyncPlan(plan, paths, runner);
        output << "APPLIED actions=" << plan.actions.size() << '\n';
        return 0;
    } catch (const BusesConfigError& error) {
        errors << "CONFIG_ERROR: " << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        errors << "MANAGER_ERROR: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace mdvwb
