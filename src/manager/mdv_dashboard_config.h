#pragma once

#include "mdv_buses_config.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mdvwb {

struct DashboardBackground {
    std::string file;
    int naturalWidth = 0;
    int naturalHeight = 0;
    double defaultScale = 1.0;
    std::string fit = "contain";
};

struct DashboardFanPlacement {
    std::string id;
    int number = 1;
    int bus = 0;
    int address = 0;
    std::string label;
    double x = 0.5;
    double y = 0.5;
    double markerScale = 1.0;
    double rotation = 0.0;
    bool visible = true;
};

// Legacy single-panel representation. It remains available for migration and
// focused tests; the manager persists DashboardCollection (version 2).
struct DashboardConfig {
    int version = 1;
    int revision = 0;
    std::string title = "Панель фанкойлов";
    DashboardBackground background;
    std::vector<DashboardFanPlacement> fans;
};

struct DashboardPanel {
    std::string id = "main";
    std::string title = "Панель фанкойлов";
    DashboardBackground background;
    std::vector<DashboardFanPlacement> fans;
};

struct DashboardCollection {
    int version = 2;
    int revision = 0;
    std::string defaultPanel = "main";
    std::vector<DashboardPanel> panels = {DashboardPanel{}};
};

enum class DashboardReferenceIssueKind {
    MissingBus,
    MissingAddress,
};

struct DashboardReferenceIssue {
    DashboardReferenceIssueKind kind = DashboardReferenceIssueKind::MissingBus;
    std::string placementId;
    int bus = 0;
    int address = 0;
    std::string panelId;
};

class DashboardConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

DashboardConfig ParseDashboardConfig(std::string_view jsonText);
DashboardConfig LoadDashboardConfig(const std::filesystem::path& path);
std::string SerializeDashboardConfig(const DashboardConfig& config);

DashboardCollection ParseDashboardCollection(std::string_view jsonText);
DashboardCollection LoadDashboardCollection(const std::filesystem::path& path);
std::string SerializeDashboardCollection(const DashboardCollection& collection);
DashboardPanel* FindDashboardPanel(DashboardCollection& collection, std::string_view id);
const DashboardPanel* FindDashboardPanel(const DashboardCollection& collection, std::string_view id);

std::vector<DashboardReferenceIssue> InspectDashboardReferences(
    const DashboardConfig& dashboard,
    const BusesConfig& buses);
std::vector<DashboardReferenceIssue> InspectDashboardReferences(
    const DashboardCollection& collection,
    const BusesConfig& buses);

}  // namespace mdvwb
