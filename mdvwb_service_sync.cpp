#include "mdvwb_service_sync.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace mdvwb {
namespace {

constexpr std::string_view ManagedMarker = "# Managed by mdvwb-manager from buses.json.";

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot read file: " + path.string());
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw std::runtime_error("cannot finish reading file: " + path.string());
    }
    return buffer.str();
}

bool TryReadFile(const std::filesystem::path& path, std::string& content) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw std::runtime_error("cannot finish reading file: " + path.string());
    }
    content = buffer.str();
    return true;
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

std::string EscapeShellValue(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        if (ch == '\\' || ch == '"' || ch == '$' || ch == '`') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::string ReplaceEnvironmentValue(
    const std::string& source,
    std::string_view key,
    std::string_view value) {
    std::istringstream input(source);
    std::ostringstream output;
    std::string line;
    bool replaced = false;

    while (std::getline(input, line)) {
        if (line.rfind(std::string(key) + "=", 0) == 0U) {
            output << key << "=\"" << EscapeShellValue(value) << "\"\n";
            replaced = true;
        } else {
            output << line << '\n';
        }
    }

    if (!replaced) {
        output << key << "=\"" << EscapeShellValue(value) << "\"\n";
    }
    return output.str();
}

std::string RenderBusEnvironment(const std::string& environmentTemplate, const BusConfig& bus) {
    std::string result(environmentTemplate);
    if (!result.empty() && result.back() != '\n') {
        result.push_back('\n');
    }
    result = ReplaceEnvironmentValue(result, "MDVWB_BUS", std::to_string(bus.id));
    result = ReplaceEnvironmentValue(result, "MDVWB_PORT", bus.port);
    result = ReplaceEnvironmentValue(result, "MDVWB_ADDRESSES", JoinAddresses(bus.addresses));
    return std::string(ManagedMarker) + "\n" + result;
}

std::filesystem::path ConfigPath(const ServiceSyncPaths& paths, int busId) {
    return paths.defaultDirectory / ("mdvwb-" + std::to_string(busId));
}

std::string FormatServiceName(int busId) {
    return "mdvwb@" + std::to_string(busId) + ".service";
}

bool IsManagedConfig(std::string_view content) {
    return content.rfind(ManagedMarker, 0) == 0U;
}

std::map<int, std::filesystem::path> FindManagedConfigs(const ServiceSyncPaths& paths) {
    std::map<int, std::filesystem::path> result;
    std::error_code error;
    if (!std::filesystem::exists(paths.defaultDirectory, error)) {
        if (error) {
            throw std::runtime_error(
                "cannot inspect configuration directory: " + error.message());
        }
        return result;
    }

    for (const auto& entry : std::filesystem::directory_iterator(paths.defaultDirectory)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        constexpr std::string_view Prefix = "mdvwb-";
        if (name.rfind(Prefix, 0) != 0U) {
            continue;
        }
        const std::string number = name.substr(Prefix.size());
        if (number.empty() ||
            !std::all_of(number.begin(), number.end(), [](unsigned char ch) { return ch >= '0' && ch <= '9'; })) {
            continue;
        }
        const int busId = std::stoi(number);
        std::string content;
        if (TryReadFile(entry.path(), content) && IsManagedConfig(content)) {
            result.emplace(busId, entry.path());
        }
    }
    return result;
}

void WriteTextFileAtomicallyImplementation(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot create temporary file: " + temporary.string());
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        if (!output) {
            throw std::runtime_error("cannot write temporary file: " + temporary.string());
        }
    }

    std::error_code error;
    std::filesystem::permissions(
        temporary,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
            std::filesystem::perms::group_read,
        std::filesystem::perm_options::replace,
        error);
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot set permissions on " + temporary.string() + ": " + error.message());
    }

    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(path, error);
        error.clear();
        std::filesystem::rename(temporary, path, error);
    }
    if (error) {
        std::filesystem::remove(temporary);
        throw std::runtime_error("cannot replace " + path.string() + ": " + error.message());
    }
}

std::string ActionName(ServiceActionType type) {
    switch (type) {
        case ServiceActionType::WriteConfig: return "WRITE_CONFIG";
        case ServiceActionType::RemoveConfig: return "REMOVE_CONFIG";
        case ServiceActionType::EnableAndStart: return "ENABLE_START";
        case ServiceActionType::EnableAndRestart: return "ENABLE_RESTART";
        case ServiceActionType::DisableAndStop: return "DISABLE_STOP";
        case ServiceActionType::EnsureEnabledAndStarted: return "ENSURE_ENABLED_STARTED";
    }
    return "UNKNOWN";
}

void RunSystemctl(
    const ServiceSyncPaths& paths,
    CommandRunner& runner,
    const std::vector<std::string>& arguments) {
    std::vector<std::string> command;
    command.reserve(arguments.size() + 1U);
    command.push_back(paths.systemctlProgram);
    command.insert(command.end(), arguments.begin(), arguments.end());
    const int code = runner.Run(command);
    if (code != 0) {
        std::ostringstream message;
        message << "systemctl command failed with code " << code << ':';
        for (const std::string& part : command) {
            message << ' ' << part;
        }
        throw std::runtime_error(message.str());
    }
}

int RunSystemctlCode(
    const ServiceSyncPaths& paths,
    CommandRunner& runner,
    const std::vector<std::string>& arguments) {
    std::vector<std::string> command;
    command.reserve(arguments.size() + 1U);
    command.push_back(paths.systemctlProgram);
    command.insert(command.end(), arguments.begin(), arguments.end());
    return runner.Run(command);
}

}  // namespace

void WriteTextFileAtomically(const std::filesystem::path& path, std::string_view content) {
    WriteTextFileAtomicallyImplementation(path, content);
}

ServiceSyncPaths ServiceSyncPathsFromEnvironment() {
    ServiceSyncPaths paths;
    if (const char* value = std::getenv("MDVWB_DEFAULT_DIR"); value != nullptr && value[0] != '\0') {
        paths.defaultDirectory = value;
    }
    if (const char* value = std::getenv("MDVWB_ENV_TEMPLATE"); value != nullptr && value[0] != '\0') {
        paths.environmentTemplate = value;
    }
    if (const char* value = std::getenv("MDVWB_SYSTEMCTL"); value != nullptr && value[0] != '\0') {
        paths.systemctlProgram = value;
    }
    return paths;
}

std::string BusServiceName(int busId) {
    if (busId < 1 || busId > 999) {
        throw std::invalid_argument("bus id must be in range 1..999");
    }
    return FormatServiceName(busId);
}

void ExecuteBusServiceCommand(
    int busId,
    BusServiceCommand command,
    const ServiceSyncPaths& paths,
    CommandRunner& runner) {
    const std::string service = BusServiceName(busId);
    switch (command) {
        case BusServiceCommand::Start:
            RunSystemctl(paths, runner, {"start", service});
            return;
        case BusServiceCommand::Stop:
            RunSystemctl(paths, runner, {"stop", service});
            return;
        case BusServiceCommand::Restart:
            RunSystemctl(paths, runner, {"restart", service});
            return;
    }
    throw std::invalid_argument("unknown bus service command");
}

BusServiceStatus QueryBusServiceStatus(
    int busId,
    const ServiceSyncPaths& paths,
    CommandRunner& runner) {
    const std::string service = BusServiceName(busId);
    BusServiceStatus status;
    status.active =
        RunSystemctlCode(paths, runner, {"is-active", "--quiet", service}) == 0;
    status.enabled =
        RunSystemctlCode(paths, runner, {"is-enabled", "--quiet", service}) == 0;
    return status;
}

ServiceSyncPlan BuildServiceSyncPlan(const BusesConfig& config, const ServiceSyncPaths& paths) {
    const std::string environmentTemplate = ReadFile(paths.environmentTemplate);
    const std::map<int, std::filesystem::path> managed = FindManagedConfigs(paths);
    std::set<int> configuredIds;
    ServiceSyncPlan plan;

    for (const BusConfig& bus : config.buses) {
        configuredIds.insert(bus.id);
        const std::filesystem::path path = ConfigPath(paths, bus.id);
        const std::string desired = RenderBusEnvironment(environmentTemplate, bus);
        std::string current;
        const bool exists = TryReadFile(path, current);
        const bool changed = !exists || current != desired;

        if (changed) {
            plan.actions.push_back({ServiceActionType::WriteConfig, bus.id, path, desired});
        }

        if (!bus.enabled) {
            plan.actions.push_back({ServiceActionType::DisableAndStop, bus.id, path, {}});
        } else if (!exists) {
            plan.actions.push_back({ServiceActionType::EnableAndStart, bus.id, path, {}});
        } else if (changed) {
            plan.actions.push_back({ServiceActionType::EnableAndRestart, bus.id, path, {}});
        } else {
            plan.actions.push_back({ServiceActionType::EnsureEnabledAndStarted, bus.id, path, {}});
        }
    }

    for (const auto& [busId, path] : managed) {
        if (configuredIds.find(busId) != configuredIds.end()) {
            continue;
        }
        plan.actions.push_back({ServiceActionType::DisableAndStop, busId, path, {}});
        plan.actions.push_back({ServiceActionType::RemoveConfig, busId, path, {}});
    }

    return plan;
}

void PrintServiceSyncPlan(const ServiceSyncPlan& plan, std::ostream& output) {
    if (plan.actions.empty()) {
        output << "NO_CHANGES\n";
        return;
    }
    for (const ServiceAction& action : plan.actions) {
        output << ActionName(action.type)
               << " bus=" << action.busId
               << " service=" << FormatServiceName(action.busId);
        if (!action.configPath.empty()) {
            output << " config=" << action.configPath.string();
        }
        output << '\n';
    }
}

void ApplyServiceSyncPlan(
    const ServiceSyncPlan& plan,
    const ServiceSyncPaths& paths,
    CommandRunner& runner) {
    for (const ServiceAction& action : plan.actions) {
        switch (action.type) {
            case ServiceActionType::WriteConfig:
                WriteTextFileAtomically(action.configPath, action.configContent);
                break;
            case ServiceActionType::RemoveConfig: {
                std::error_code error;
                std::filesystem::remove(action.configPath, error);
                if (error) {
                    throw std::runtime_error(
                        "cannot remove " + action.configPath.string() + ": " + error.message());
                }
                break;
            }
            case ServiceActionType::EnableAndStart:
                RunSystemctl(paths, runner, {"enable", "--now", FormatServiceName(action.busId)});
                break;
            case ServiceActionType::EnableAndRestart:
                RunSystemctl(paths, runner, {"enable", FormatServiceName(action.busId)});
                RunSystemctl(paths, runner, {"restart", FormatServiceName(action.busId)});
                break;
            case ServiceActionType::DisableAndStop:
                RunSystemctl(paths, runner, {"disable", "--now", FormatServiceName(action.busId)});
                break;
            case ServiceActionType::EnsureEnabledAndStarted:
                RunSystemctl(paths, runner, {"enable", "--now", FormatServiceName(action.busId)});
                break;
        }
    }
}

int NativeCommandRunner::Run(const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        return 127;
    }
#ifdef _WIN32
    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 1U);
    for (const std::string& argument : arguments) {
        argv.push_back(argument.c_str());
    }
    argv.push_back(nullptr);
    return _spawnvp(_P_WAIT, argv.front(), argv.data());
#else
    const pid_t child = fork();
    if (child < 0) {
        return 127;
    }
    if (child == 0) {
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1U);
        for (const std::string& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        execvp(argv.front(), argv.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        return 127;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return 128;
#endif
}

}  // namespace mdvwb
