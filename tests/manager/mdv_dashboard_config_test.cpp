#include "mdv_dashboard_config.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void Fail(const std::string& message) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
}

void Expect(bool condition, const std::string& message) {
    if (!condition) {
        Fail(message);
    }
}

template <typename Function>
void ExpectConfigError(Function&& function, std::string_view expectedText) {
    try {
        function();
        Fail("expected DashboardConfigError containing: " + std::string(expectedText));
    } catch (const mdvwb::DashboardConfigError& error) {
        if (std::string(error.what()).find(expectedText) == std::string::npos) {
            Fail("unexpected error: " + std::string(error.what()));
        }
    }
}

const char* kValidConfig = R"JSON({
  "version": 1,
  "revision": 4,
  "title": "М22, третий этаж",
  "background": {
    "file": "background-a1b2c3.webp",
    "naturalWidth": 2400,
    "naturalHeight": 1600,
    "defaultScale": 1.25,
    "fit": "custom"
  },
  "fans": [
    {
      "id": "fan-2-18",
      "number": 18,
      "bus": 2,
      "address": 18,
      "label": "Переговорная",
      "x": 0.735,
      "y": 0.182,
      "markerScale": 1.1,
      "rotation": -15,
      "visible": true
    },
    {
      "id": "fan-1-3",
      "number": 5,
      "bus": 1,
      "address": 3,
      "label": "Фанкойл №5",
      "x": 0.245,
      "y": 0.418,
      "markerScale": 1,
      "rotation": 0,
      "visible": true
    }
  ]
})JSON";

void TestParseAndCanonicalSerialize() {
    const mdvwb::DashboardConfig config = mdvwb::ParseDashboardConfig(kValidConfig);
    Expect(config.version == 1, "version");
    Expect(config.revision == 4, "revision");
    Expect(config.background.naturalWidth == 2400, "background width");
    Expect(config.fans.size() == 2U, "fan count");
    Expect(config.fans[0].number == 18, "user number");
    Expect(config.fans[0].bus == 2, "input order remains available after parse");

    const std::string serialized = mdvwb::SerializeDashboardConfig(config);
    const std::size_t first = serialized.find("\"id\": \"fan-1-3\"");
    const std::size_t second = serialized.find("\"id\": \"fan-2-18\"");
    Expect(first != std::string::npos && second != std::string::npos && first < second,
           "canonical serializer sorts by bus/address");

    const mdvwb::DashboardConfig roundTrip = mdvwb::ParseDashboardConfig(serialized);
    Expect(roundTrip.fans.size() == 2U, "round trip fan count");
    Expect(roundTrip.background.defaultScale == 1.25, "round trip scale");
    Expect(roundTrip.fans[0].number == 5, "canonical number round trip");
}

void TestLegacyNumbersAreAssignedSequentially() {
    const auto config = mdvwb::ParseDashboardConfig(R"JSON({
      "version": 1,
      "revision": 0,
      "title": "Старая панель",
      "background": {
        "file": "",
        "naturalWidth": 0,
        "naturalHeight": 0,
        "defaultScale": 1,
        "fit": "contain"
      },
      "fans": [
        {"id":"a","bus":1,"address":1,"label":"A","x":0,"y":0,"markerScale":1,"rotation":0,"visible":true},
        {"id":"b","bus":1,"address":2,"label":"B","x":1,"y":1,"markerScale":1,"rotation":0,"visible":true}
      ]
    })JSON");
    Expect(config.fans[0].number == 1 && config.fans[1].number == 2,
           "legacy placements receive sequential user numbers");
    Expect(mdvwb::SerializeDashboardConfig(config).find("\"number\": 1") != std::string::npos,
           "canonical serialization persists assigned number");
}

void TestEmptyBackground() {
    const auto config = mdvwb::ParseDashboardConfig(R"JSON({
      "version": 1,
      "revision": 0,
      "title": "Новая панель",
      "background": {
        "file": "",
        "naturalWidth": 0,
        "naturalHeight": 0,
        "defaultScale": 1,
        "fit": "contain"
      },
      "fans": []
    })JSON");
    Expect(config.background.file.empty(), "empty background is allowed");
}

void TestStrictValidation() {
    ExpectConfigError([] {
        mdvwb::ParseDashboardConfig(R"JSON({
          "version": 1, "revision": 0, "title": "X",
          "background": {"file":"plan.svg","naturalWidth":100,"naturalHeight":100,"defaultScale":1,"fit":"contain"},
          "fans": []
        })JSON");
    }, "PNG/JPEG/WebP");

    ExpectConfigError([] {
        mdvwb::ParseDashboardConfig(R"JSON({
          "version": 1, "revision": 0, "title": "X",
          "background": {"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
          "fans": [{"id":"a","bus":1,"address":1,"label":"A","x":1.1,"y":0,"markerScale":1,"rotation":0,"visible":true}]
        })JSON");
    }, "dashboard.fans[0].x");

    ExpectConfigError([] {
        mdvwb::ParseDashboardConfig(R"JSON({
          "version": 1, "revision": 0, "title": "X",
          "background": {"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
          "fans": [
            {"id":"a","bus":1,"address":1,"label":"A","x":0,"y":0,"markerScale":1,"rotation":0,"visible":true},
            {"id":"b","bus":1,"address":1,"label":"B","x":1,"y":1,"markerScale":1,"rotation":0,"visible":true}
          ]
        })JSON");
    }, "duplicate device");

    ExpectConfigError([] {
        mdvwb::ParseDashboardConfig(R"JSON({
          "version": 1, "revision": 0, "title": "X",
          "background": {"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
          "fans": [
            {"id":"a","number":7,"bus":1,"address":1,"label":"A","x":0,"y":0,"markerScale":1,"rotation":0,"visible":true},
            {"id":"b","number":7,"bus":1,"address":2,"label":"B","x":1,"y":1,"markerScale":1,"rotation":0,"visible":true}
          ]
        })JSON");
    }, "duplicate number 7");

    ExpectConfigError([] {
        mdvwb::ParseDashboardConfig(R"JSON({
          "version": 1, "revision": 0, "title": "X", "unexpected": true,
          "background": {"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
          "fans": []
        })JSON");
    }, "unknown field");
}

void TestReferenceInspection() {
    const mdvwb::DashboardConfig dashboard = mdvwb::ParseDashboardConfig(kValidConfig);
    mdvwb::BusesConfig buses;
    buses.buses = {
        mdvwb::BusConfig{1, true, "/dev/ttyRS485-1", {1, 2}},
    };

    const auto issues = mdvwb::InspectDashboardReferences(dashboard, buses);
    Expect(issues.size() == 2U, "two stale references expected");
    Expect(issues[0].kind == mdvwb::DashboardReferenceIssueKind::MissingBus,
           "first input placement points to a missing bus");
    Expect(issues[1].kind == mdvwb::DashboardReferenceIssueKind::MissingAddress,
           "second input placement points to a missing address");

    // Missing references are warnings, not parse errors: the editor must let the
    // user repair or remove a stale marker after buses.json changes.
    Expect(dashboard.fans.size() == 2U, "stale placements remain in dashboard config");
}


void TestCollectionMigrationAndRoundTrip() {
    const mdvwb::DashboardCollection migrated = mdvwb::ParseDashboardCollection(kValidConfig);
    Expect(migrated.version == 2, "legacy dashboard migrates to collection version 2");
    Expect(migrated.revision == 4, "legacy collection revision");
    Expect(migrated.defaultPanel == "main", "legacy default panel");
    Expect(migrated.panels.size() == 1U && migrated.panels[0].fans.size() == 2U,
           "legacy dashboard became main panel");

    const auto collection = mdvwb::ParseDashboardCollection(R"JSON({
      "version": 2,
      "revision": 9,
      "defaultPanel": "main",
      "panels": [
        {
          "id": "main",
          "title": "Главный корпус",
          "background": {"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},
          "fans": [
            {"id":"fan-1-1","number":1,"bus":1,"address":1,"label":"Приёмная","x":0.2,"y":0.3,"markerScale":1,"rotation":0,"visible":true}
          ]
        },
        {
          "id": "floor-2",
          "title": "Второй этаж",
          "background": {"file":"background-floor2.webp","naturalWidth":1600,"naturalHeight":900,"defaultScale":1.2,"fit":"custom"},
          "fans": [
            {"id":"fan-2-18","number":101,"bus":2,"address":18,"label":"Кабинет 201","x":0.6,"y":0.5,"markerScale":1,"rotation":0,"visible":true}
          ]
        }
      ]
    })JSON");
    Expect(collection.panels.size() == 2U, "two panels parsed");
    Expect(collection.panels[1].id == "floor-2", "second panel id");
    Expect(collection.panels[1].fans[0].number == 101, "panel-local number");
    Expect(mdvwb::FindDashboardPanel(collection, "floor-2") != nullptr,
           "find panel by id");

    const std::string serialized = mdvwb::SerializeDashboardCollection(collection);
    const auto roundTrip = mdvwb::ParseDashboardCollection(serialized);
    Expect(roundTrip.defaultPanel == "main" && roundTrip.panels.size() == 2U,
           "collection round trip");
    Expect(roundTrip.panels[1].background.file == "background-floor2.webp",
           "independent panel background round trip");
}

void TestCollectionValidationAndReferences() {
    ExpectConfigError([] {
        mdvwb::ParseDashboardCollection(R"JSON({
          "version":2,"revision":0,"defaultPanel":"missing",
          "panels":[{"id":"main","title":"Main","background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},"fans":[]}]
        })JSON");
    }, "defaultPanel");

    ExpectConfigError([] {
        mdvwb::ParseDashboardCollection(R"JSON({
          "version":2,"revision":0,"defaultPanel":"main",
          "panels":[
            {"id":"main","title":"A","background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},"fans":[]},
            {"id":"main","title":"B","background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},"fans":[]}
          ]
        })JSON");
    }, "duplicate id");

    const auto collection = mdvwb::ParseDashboardCollection(R"JSON({
      "version":2,"revision":0,"defaultPanel":"main",
      "panels":[
        {"id":"main","title":"A","background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},"fans":[{"id":"a","number":1,"bus":9,"address":1,"label":"A","x":0,"y":0,"markerScale":1,"rotation":0,"visible":true}]},
        {"id":"floor-2","title":"B","background":{"file":"","naturalWidth":0,"naturalHeight":0,"defaultScale":1,"fit":"contain"},"fans":[{"id":"b","number":1,"bus":1,"address":63,"label":"B","x":1,"y":1,"markerScale":1,"rotation":0,"visible":true}]}
      ]
    })JSON");
    mdvwb::BusesConfig buses;
    buses.buses = {mdvwb::BusConfig{1, true, "/dev/ttyRS485-1", {1}}};
    const auto issues = mdvwb::InspectDashboardReferences(collection, buses);
    Expect(issues.size() == 2U, "collection reference issues");
    Expect(issues[0].panelId == "main" && issues[1].panelId == "floor-2",
           "reference issue identifies panel");
}

}  // namespace

int main() {
    TestParseAndCanonicalSerialize();
    TestLegacyNumbersAreAssignedSequentially();
    TestEmptyBackground();
    TestStrictValidation();
    TestReferenceInspection();
    TestCollectionMigrationAndRoundTrip();
    TestCollectionValidationAndReferences();
    std::cout << "MDVWB dashboard config tests: OK\n";
    return 0;
}
