#pragma once

#include "mdv_buses_config.h"
#include "mdv_dashboard_config.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mdvwb {

enum class ScheduleKind {
    Weekly,
    Once,
};

struct ScheduleTarget {
    int bus = 0;
    int address = 0;
};

struct ScheduleActions {
    std::optional<bool> power;
    std::optional<int> mode;
    std::optional<int> speed;
    std::optional<int> setTemp;
};

struct ScheduleEntry {
    std::string id;
    std::string name;
    bool enabled = true;
    std::string panelId = "main";
    ScheduleKind kind = ScheduleKind::Weekly;
    std::vector<int> days;
    std::string date;
    std::string time = "00:00";
    std::vector<ScheduleTarget> targets;
    ScheduleActions actions;
};

struct SchedulesConfig {
    int version = 1;
    int revision = 0;
    std::vector<ScheduleEntry> schedules;
};

enum class ScheduleReferenceIssueKind {
    MissingPanel,
    MissingBus,
    MissingAddress,
    TargetNotInPanel,
};

struct ScheduleReferenceIssue {
    ScheduleReferenceIssueKind kind = ScheduleReferenceIssueKind::MissingPanel;
    std::string scheduleId;
    std::string panelId;
    int bus = 0;
    int address = 0;
};

class SchedulesConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

SchedulesConfig ParseSchedulesConfig(std::string_view jsonText);
SchedulesConfig LoadSchedulesConfig(const std::filesystem::path& path);
std::string SerializeSchedulesConfig(const SchedulesConfig& config);
const ScheduleEntry* FindSchedule(const SchedulesConfig& config, std::string_view id);
ScheduleEntry* FindSchedule(SchedulesConfig& config, std::string_view id);

std::vector<ScheduleReferenceIssue> InspectScheduleReferences(
    const SchedulesConfig& schedules,
    const BusesConfig& buses,
    const DashboardCollection& dashboards);

void ValidateScheduleReferences(
    const SchedulesConfig& schedules,
    const BusesConfig& buses,
    const DashboardCollection& dashboards);

}  // namespace mdvwb
