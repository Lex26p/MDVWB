#include "mdvwb_scheduler.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

std::string Env(const char* name, std::string fallback) {
    const char* value = std::getenv(name);
    return value == nullptr || value[0] == '\0' ? std::move(fallback) : std::string(value);
}

int EnvInt(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }
    return std::stoi(value);
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout
            << "MDVWB schedule executor\n\n"
            << "Configuration is read from MDVWB_SCHEDULES_CONFIG, MDVWB_BUSES_CONFIG,\n"
            << "MDVWB_DASHBOARD_CONFIG and MDVWB_SCHEDULER_STATE.\n";
        return 0;
    }
    if (argc > 1) {
        std::cerr << "SCHEDULER_ERROR: unknown argument\n";
        return 2;
    }

    mdvwb::SchedulerPaths paths;
    paths.schedules = Env("MDVWB_SCHEDULES_CONFIG", "/etc/mdvwb/schedules.json");
    paths.buses = Env("MDVWB_BUSES_CONFIG", "/etc/mdvwb/buses.json");
    paths.dashboard = Env("MDVWB_DASHBOARD_CONFIG", "/etc/mdvwb/dashboard.json");
    paths.state = Env("MDVWB_SCHEDULER_STATE", "/var/lib/mdvwb/scheduler-state.tsv");
    paths.confirmationTimeoutSeconds = EnvInt("MDVWB_SCHEDULER_CONFIRM_TIMEOUT", 10);
    return mdvwb::RunSchedulerDaemon(paths, std::cout, std::cerr);
}
