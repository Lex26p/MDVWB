import assert from "node:assert/strict";
import {
  availableDashboardDevices,
  cloneDashboardCollection,
  cloneDashboardConfiguration,
  createFanPlacement,
  dashboardCollectionToJson,
  dashboardConfigurationToPanel,
  dashboardAssetUrl,
  dashboardConfigurationToJson,
  deviceKey,
  findDashboardPanel,
  inspectDashboardPlacement,
  nextPanelIdentifier,
  normalizeDashboardCollection,
  normalizeDashboardConfiguration,
  panelUserUrl,
  safeUploadFileName,
  sha256HexBytes,
} from "../../www/mdvwb/dashboard-model.js";

const dashboard = normalizeDashboardConfiguration({
  version: 1,
  revision: 4,
  title: "Корпус 1",
  background: {
    file: "background-0123456789abcdef.webp",
    naturalWidth: 2400,
    naturalHeight: 1600,
    defaultScale: 1.25,
    fit: "contain",
  },
  fans: [
    {
      id: "fan-1-3",
      number: 3,
      bus: 1,
      address: 3,
      label: "Фанкойл №3",
      x: 0.25,
      y: 0.5,
      markerScale: 1,
      rotation: 45,
      visible: true,
    },
  ],
});

assert.equal(dashboard.revision, 4);
assert.equal(dashboard.fans[0].number, 3);
assert.equal(dashboard.fans[0].rotation, 0);
assert.equal(dashboard.background.defaultScale, 1.25);
assert.equal(dashboardAssetUrl(dashboard.background.file), "/fancoils/assets/background-0123456789abcdef.webp");
assert.equal(safeUploadFileName("План этажа №3.webp"), "No3.webp");
assert.equal(
  sha256HexBytes(new TextEncoder().encode("abc")),
  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
);

const clone = cloneDashboardConfiguration(dashboard);
clone.title = "Изменено";
assert.equal(dashboard.title, "Корпус 1");
assert.equal(JSON.parse(dashboardConfigurationToJson(dashboard)).fans[0].address, 3);

const buses = {
  version: 1,
  buses: [
    { id: 1, enabled: true, port: "/dev/ttyRS485-1", addresses: [1, 3] },
    { id: 2, enabled: false, port: "/dev/ttyUSB0", addresses: [18] },
  ],
};
const devices = availableDashboardDevices(buses);
assert.deepEqual(devices.map((device) => device.key), ["1:1", "1:3", "2:18"]);
assert.equal(deviceKey(2, 18), "2:18");
assert.equal(inspectDashboardPlacement(dashboard.fans[0], buses), null);
assert.equal(
  inspectDashboardPlacement({ bus: 9, address: 1 }, buses).kind,
  "missingBus",
);
assert.equal(
  inspectDashboardPlacement({ bus: 1, address: 63 }, buses).kind,
  "missingAddress",
);

const placement = createFanPlacement(
  { bus: 2, address: 18, label: "Переговорная", x: 0.7, y: 0.2 },
  dashboard.fans,
);
assert.equal(placement.id, "fan-2-18");
assert.equal(placement.number, 1);
assert.equal(placement.markerScale, 1);
assert.throws(
  () => createFanPlacement({ bus: 1, address: 3 }, dashboard.fans),
  /уже размещено/,
);

assert.throws(() => normalizeDashboardConfiguration({
  ...dashboard,
  fans: [dashboard.fans[0], { ...dashboard.fans[0], id: "other" }],
}), /повторно/);

const legacy = normalizeDashboardConfiguration({
  ...dashboard,
  fans: [{ ...dashboard.fans[0], number: undefined, label: "Фанкойл №17" }],
});
assert.equal(legacy.fans[0].number, 17);
assert.throws(() => normalizeDashboardConfiguration({
  ...dashboard,
  fans: [dashboard.fans[0], { ...dashboard.fans[0], id: "other", bus: 2, address: 18 }],
}), /Номер фанкойла 3 указан повторно/);


const migratedCollection = normalizeDashboardCollection(dashboard);
assert.equal(migratedCollection.version, 2);
assert.equal(migratedCollection.defaultPanel, "main");
assert.equal(migratedCollection.panels.length, 1);
assert.equal(migratedCollection.panels[0].title, "Корпус 1");

const collection = normalizeDashboardCollection({
  version: 2,
  revision: 9,
  defaultPanel: "main",
  panels: [
    dashboardConfigurationToPanel(dashboard, "main"),
    {
      id: "floor-2",
      title: "Второй этаж",
      background: { file: "", naturalWidth: 0, naturalHeight: 0, defaultScale: 1, fit: "contain" },
      fans: [{ ...dashboard.fans[0], id: "fan-1-3-floor2", number: 101, label: "Кабинет 201" }],
    },
  ],
});
assert.equal(collection.panels.length, 2);
assert.equal(collection.panels[1].fans[0].number, 101);
assert.equal(panelUserUrl("floor-2"), "/fancoils/?panel=floor-2");
assert.equal(nextPanelIdentifier(collection), "panel-1");
const collectionClone = cloneDashboardCollection(collection);
collectionClone.panels[1].title = "Изменено";
assert.equal(collection.panels[1].title, "Второй этаж");
const mutablePanel = findDashboardPanel(collectionClone, "floor-2");
mutablePanel.title = "Изменено через поиск";
assert.equal(collectionClone.panels[1].title, "Изменено через поиск");
assert.equal(JSON.parse(dashboardCollectionToJson(collection)).version, 2);
assert.throws(() => normalizeDashboardCollection({ ...collection, panels: [collection.panels[0], { ...collection.panels[0] }] }), /указан повторно/);
assert.throws(() => normalizeDashboardCollection({ ...collection, defaultPanel: "missing" }), /не существует/);

console.log("MDVWB dashboard web model tests: OK");
