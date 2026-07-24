import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import {
  alarmLabel,
  applyFanControl,
  buildGroupCommandPlan,
  canSendFanCommands,
  clampScale,
  computeFitScale,
  createFanState,
  dashboardFromPayload,
  dashboardSelectionFromPayload,
  fanCommandMatchesState,
  fanCommandTopic,
  fanDeviceKey,
  markerMatchesFilter,
  normalizeFanCommand,
  normalizeGroupCommands,
  parseFanControlTopic,
  statusClass,
  statusLabel,
  summarizeDashboard,
  temperatureLabel,
} from "../../www/fancoils/model.js";

assert.deepEqual(
  parseFanControlTopic("/devices/Fan-2_18/controls/Temp"),
  { bus: 2, address: 18, control: "Temp", key: "2:18", device: "Fan-2_18" },
);
assert.equal(parseFanControlTopic("/devices/Fan-0_1/controls/Temp"), null);
assert.equal(parseFanControlTopic("/devices/Fan-2_64/controls/Temp"), null);
assert.equal(parseFanControlTopic("/devices/Fan-2_18/controls/Unknown"), null);
assert.equal(parseFanControlTopic("/devices/Fan-2_18/controls/Power/on1"), null);

let fan = createFanState(2, 18);
fan = applyFanControl(fan, "Status", "2", 1234);
fan = applyFanControl(fan, "Temp", "21.5", 1235);
fan = applyFanControl(fan, "Alarm", "0", 1236);
assert.equal(fan.Status, 2);
assert.equal(fan.Temp, 21.5);
assert.equal(fan.updatedAt, 1236);
assert.equal(statusLabel(fan.Status), "Нагрев");
assert.equal(statusClass(fan.Status), "heating");
assert.equal(temperatureLabel(fan.Temp), "21.5 °C");
assert.equal(alarmLabel(fan), "Нет аварии");
assert.equal(markerMatchesFilter(fan, "online"), true);
assert.equal(markerMatchesFilter(fan, "alarm"), false);

let alarm = createFanState(1, 3);
alarm = applyFanControl(alarm, "Status", 6);
alarm = applyFanControl(alarm, "Alarm", 1);
alarm = applyFanControl(alarm, "AlarmCode", 9);
assert.equal(alarmLabel(alarm), "Авария, код 9");
assert.equal(markerMatchesFilter(alarm, "alarm"), true);

let offline = createFanState(3, 2);
offline = applyFanControl(offline, "Status", 7);
offline = applyFanControl(offline, "Alarm", 2);
assert.equal(markerMatchesFilter(offline, "offline"), true);
assert.equal(alarmLabel(offline), "Нет связи");

assert.equal(fanCommandTopic(2, 18, "Power"), "/devices/Fan-2_18/controls/Power/on1");
assert.equal(normalizeFanCommand("Mode", "4"), 4);
assert.equal(normalizeFanCommand("Speed", 1), 1);
assert.equal(normalizeFanCommand("SetTemp", 32), 32);
assert.equal(normalizeFanCommand("Blok", 0), 0);
assert.throws(() => normalizeFanCommand("SetTemp", 15));
assert.throws(() => normalizeFanCommand("Temp", 22));
assert.throws(() => fanCommandTopic(0, 1, "Power"));
fan = applyFanControl(fan, "Mode", 1);
assert.equal(canSendFanCommands(true, fan), true);
assert.equal(canSendFanCommands(false, fan), false);
assert.equal(canSendFanCommands(true, offline), false);
assert.equal(fanCommandMatchesState("Mode", 1, fan), true);
assert.equal(fanCommandMatchesState("Mode", 0, fan), false);

const dashboard = dashboardFromPayload(JSON.stringify({
  version: 1,
  revision: 4,
  title: "Корпус 1",
  background: { file: "", naturalWidth: 1600, naturalHeight: 900, defaultScale: 1, fit: "contain" },
  fans: [
    { id: "fan-2-18", bus: 2, address: 18, label: "Переговорная", x: 0.2, y: 0.3, markerScale: 1, rotation: 0, visible: true },
    { id: "fan-1-3", bus: 1, address: 3, label: "Кабинет", x: 0.5, y: 0.5, markerScale: 1, rotation: 0, visible: true },
    { id: "fan-3-2", bus: 3, address: 2, label: "Архив", x: 0.7, y: 0.7, markerScale: 1, rotation: 0, visible: true },
    { id: "fan-4-1", bus: 4, address: 1, label: "Скрытый", x: 0.8, y: 0.8, markerScale: 1, rotation: 0, visible: false },
  ],
}));

const summary = summarizeDashboard(dashboard, new Map([
  [fanDeviceKey(2, 18), fan],
  [fanDeviceKey(1, 3), alarm],
  [fanDeviceKey(3, 2), offline],
]));
assert.deepEqual(summary, { placed: 4, visible: 3, online: 2, alarms: 1, offline: 1, waiting: 0 });

assert.equal(clampScale(0), 1);
assert.equal(clampScale(8), 4);
assert.equal(computeFitScale("width", 1600, 900, 832, 600), 0.5);
assert.equal(computeFitScale("custom", 1600, 900, 832, 600, 1.25), 1.25);
assert.equal(computeFitScale("actual", 1600, 900, 832, 600), 1);
assert.equal(computeFitScale("contain", 1600, 900, 832, 482), 0.5);


assert.deepEqual(normalizeGroupCommands({
  Power: { enabled: true, value: "1" },
  Mode: { enabled: false, value: "0" },
  Speed: { enabled: true, value: "4" },
  SetTemp: { enabled: true, value: "24" },
}), [
  { control: "Power", value: 1 },
  { control: "Speed", value: 4 },
  { control: "SetTemp", value: 24 },
]);
assert.throws(() => normalizeGroupCommands({}));

const groupPlan = buildGroupCommandPlan(
  dashboard,
  new Set([fanDeviceKey(2, 18), fanDeviceKey(1, 3), fanDeviceKey(3, 2)]),
  new Map([
    [fanDeviceKey(2, 18), fan],
    [fanDeviceKey(1, 3), alarm],
    [fanDeviceKey(3, 2), offline],
  ]),
  true,
  {
    Power: { enabled: true, value: 1 },
    Mode: { enabled: false, value: 0 },
    Speed: { enabled: false, value: 4 },
    SetTemp: { enabled: true, value: 23 },
  },
  new Set([`${fanDeviceKey(1, 3)}:Power`]),
);
assert.equal(groupPlan.targets.length, 2);
assert.equal(groupPlan.skipped.length, 1);
assert.equal(groupPlan.pendingSkipped.length, 1);
assert.equal(groupPlan.operations.length, 3);
assert.equal(groupPlan.operations[0].topic, "/devices/Fan-2_18/controls/Power/on1");
assert.equal(groupPlan.operations.every((operation) => operation.control === "Power" || operation.control === "SetTemp"), true);


const panelPayload = JSON.stringify({
  version: 2,
  revision: 3,
  defaultPanel: "main",
  panels: [
    { id: "main", title: "Главная", background: { file: "", naturalWidth: 0, naturalHeight: 0, defaultScale: 1, fit: "contain" }, fans: [] },
    { id: "floor-2", title: "Второй этаж", background: { file: "", naturalWidth: 0, naturalHeight: 0, defaultScale: 1, fit: "contain" }, fans: [{ id: "fan-2-18", number: 101, bus: 2, address: 18, label: "Кабинет 201", x: .5, y: .5, markerScale: 1, rotation: 0, visible: true }] },
  ],
});
const selectedPanel = dashboardSelectionFromPayload(panelPayload, "floor-2");
assert.equal(selectedPanel.panelId, "floor-2");
assert.equal(selectedPanel.requestedFound, true);
assert.equal(selectedPanel.dashboard.title, "Второй этаж");
assert.equal(selectedPanel.dashboard.fans[0].number, 101);
const fallbackPanel = dashboardSelectionFromPayload(panelPayload, "missing");
assert.equal(fallbackPanel.panelId, "main");
assert.equal(fallbackPanel.requestedFound, false);
assert.equal(dashboardFromPayload(panelPayload, "floor-2").title, "Второй этаж");

const pageHtml = readFileSync(new URL("../../www/fancoils/index.html", import.meta.url), "utf8");
const pageCss = readFileSync(new URL("../../www/fancoils/styles.css", import.meta.url), "utf8");
assert.match(pageHtml, /id="scheduleButton"/);
assert.match(pageHtml, /id="groupModeButton"/);
assert.doesNotMatch(pageHtml, /href="\/mdvwb\//);
assert.doesNotMatch(pageHtml, />Редактор</);
assert.doesNotMatch(pageHtml, /Жалюзи|Блокировка/);
assert.match(pageCss, /--header-height:\s*56px/);
assert.match(pageCss, /user-select:\s*none/);

console.log("MDVWB fan-coil working panel model tests: OK");
