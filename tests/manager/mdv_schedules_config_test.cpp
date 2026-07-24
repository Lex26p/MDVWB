#include "mdv_schedules_config.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void Require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

template <typename Function>
void RequireError(Function&& function, std::string_view fragment) {
    try {
        function();
    } catch (const mdvwb::SchedulesConfigError& error) {
        Require(std::string_view(error.what()).find(fragment) != std::string_view::npos,
                "error message does not contain expected fragment");
        return;
    }
    throw std::runtime_error("expected SchedulesConfigError");
}

std::string ValidConfig() {
    return R"json({
      "version":1,
      "revision":4,
      "schedules":[
        {
          "id":"workday-start",
          "name":"Начало рабочего дня",
          "enabled":true,
          "panelId":"main",
          "kind":"weekly",
          "days":[5,1,3,2,4],
          "date":"",
          "time":"08:15",
          "targets":[{"bus":2,"address":18},{"bus":1,"address":1}],
          "actions":{"power":true,"mode":0,"speed":2,"setTemp":23}
        },
        {
          "id":"holiday-off",
          "name":"Разовое выключение",
          "enabled":false,
          "panelId":"floor-2",
          "kind":"once",
          "days":[],
          "date":"2026-12-31",
          "time":"18:00",
          "targets":[{"bus":2,"address":18}],
          "actions":{"power":false}
        }
      ]
    })json";
}

void TestParseAndCanonicalSerialization() {
    const mdvwb::SchedulesConfig config = mdvwb::ParseSchedulesConfig(ValidConfig());
    Require(config.version == 1 && config.revision == 4,
            "root fields were not parsed");
    Require(config.schedules.size() == 2U,
            "schedule count is wrong");
    const auto* weekly = mdvwb::FindSchedule(config, "workday-start");
    Require(weekly != nullptr && weekly->kind == mdvwb::ScheduleKind::Weekly,
            "weekly schedule is missing");
    Require(weekly->days == std::vector<int>({1, 2, 3, 4, 5}),
            "weekdays were not canonicalized");
    Require(weekly->targets.front().bus == 1 && weekly->targets.front().address == 1,
            "targets were not canonicalized");
    Require(weekly->actions.power == true && weekly->actions.mode == 0 &&
                weekly->actions.speed == 2 && weekly->actions.setTemp == 23,
            "actions were not parsed");

    const std::string serialized = mdvwb::SerializeSchedulesConfig(config);
    const mdvwb::SchedulesConfig reparsed = mdvwb::ParseSchedulesConfig(serialized);
    Require(reparsed.schedules.size() == 2U &&
                mdvwb::FindSchedule(reparsed, "holiday-off") != nullptr,
            "serialized config cannot be parsed");
    Require(serialized.find("\"days\": [1, 2, 3, 4, 5]") != std::string::npos,
            "canonical days are missing from serialized config");
}

void TestValidationRejectsInvalidFields() {
    RequireError([] {
        mdvwb::ParseSchedulesConfig(R"json({"version":1,"revision":0,"schedules":[{
          "id":"x","name":"X","enabled":true,"panelId":"main","kind":"weekly",
          "days":[],"date":"","time":"08:00","targets":[{"bus":1,"address":1}],
          "actions":{"power":true}}]})json");
    }, "days must not be empty");

    RequireError([] {
        mdvwb::ParseSchedulesConfig(R"json({"version":1,"revision":0,"schedules":[{
          "id":"x","name":"X","enabled":true,"panelId":"main","kind":"once",
          "days":[],"date":"2026-02-30","time":"08:00","targets":[{"bus":1,"address":1}],
          "actions":{"power":true}}]})json");
    }, "valid YYYY-MM-DD");

    RequireError([] {
        mdvwb::ParseSchedulesConfig(R"json({"version":1,"revision":0,"schedules":[{
          "id":"x","name":"X","enabled":true,"panelId":"main","kind":"weekly",
          "days":[1],"date":"","time":"24:00","targets":[{"bus":1,"address":1}],
          "actions":{"power":true}}]})json");
    }, "HH:MM");

    RequireError([] {
        mdvwb::ParseSchedulesConfig(R"json({"version":1,"revision":0,"schedules":[{
          "id":"x","name":"X","enabled":true,"panelId":"main","kind":"weekly",
          "days":[1],"date":"","time":"08:00","targets":[{"bus":1,"address":1}],
          "actions":{}}]})json");
    }, "at least one action");

    RequireError([] {
        mdvwb::ParseSchedulesConfig(R"json({"version":1,"revision":0,"schedules":[{
          "id":"x","name":"X","enabled":true,"panelId":"main","kind":"weekly",
          "days":[1],"date":"","time":"08:00",
          "targets":[{"bus":1,"address":1},{"bus":1,"address":1}],
          "actions":{"setTemp":23}}]})json");
    }, "duplicates target");
}

void TestReferenceValidation() {
    const mdvwb::SchedulesConfig config = mdvwb::ParseSchedulesConfig(ValidConfig());
    const mdvwb::BusesConfig buses = mdvwb::ParseBusesConfig(R"json({
      "version":1,"buses":[
        {"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1]},
        {"id":2,"enabled":true,"port":"/dev/ttyRS485-2","addresses":[18]}
      ]})json");
    const mdvwb::DashboardCollection dashboards = mdvwb::ParseDashboardCollection(R"json({
      "version":2,"revision":0,"defaultPanel":"main","panels":[
        {"id":"main","title":"Main","background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
         "fans":[
          {"id":"fan-1-1","number":1,"bus":1,"address":1,"label":"A","x":0.2,"y":0.2,"markerScale":1,"rotation":0,"visible":true},
          {"id":"fan-2-18","number":2,"bus":2,"address":18,"label":"B","x":0.4,"y":0.4,"markerScale":1,"rotation":0,"visible":true}]},
        {"id":"floor-2","title":"Floor 2","background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
         "fans":[{"id":"fan-2-18","number":101,"bus":2,"address":18,"label":"B","x":0.5,"y":0.5,"markerScale":1,"rotation":0,"visible":true}]}
      ]})json");

    Require(mdvwb::InspectScheduleReferences(config, buses, dashboards).empty(),
            "valid references were reported as broken");
    mdvwb::ValidateScheduleReferences(config, buses, dashboards);

    mdvwb::DashboardCollection broken = dashboards;
    mdvwb::FindDashboardPanel(broken, "floor-2")->fans.clear();
    const auto issues = mdvwb::InspectScheduleReferences(config, buses, broken);
    Require(issues.size() == 1U &&
                issues.front().kind == mdvwb::ScheduleReferenceIssueKind::TargetNotInPanel,
            "missing panel target issue was not detected");
    RequireError([&] { mdvwb::ValidateScheduleReferences(config, buses, broken); },
                 "not visible in panel");
}

void TestFileRoundTrip() {
    const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("mdvwb-schedules-test-" + std::to_string(token) + ".json");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << ValidConfig();
    }
    const auto loaded = mdvwb::LoadSchedulesConfig(path);
    std::error_code error;
    std::filesystem::remove(path, error);
    Require(loaded.revision == 4 && loaded.schedules.size() == 2U,
            "file configuration was not loaded");
}

}  // namespace

int main() {
    try {
        TestParseAndCanonicalSerialization();
        TestValidationRejectsInvalidFields();
        TestReferenceValidation();
        TestFileRoundTrip();
        std::cout << "MDVWB schedules configuration tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "MDVWB schedules configuration tests: FAILED: " << error.what() << '\n';
        return 1;
    }
}
