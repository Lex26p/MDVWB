import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import {
  cloneSchedule,
  createSchedule,
  nextRunLabel,
  nextScheduleIdentifier,
  nextScheduleRun,
  normalizeSchedule,
  normalizeSchedulesConfiguration,
  parseScheduleResultTopic,
  scheduleTargetKey,
  scheduleTimingLabel,
  schedulesConfigurationToJson,
  schedulesForPanel,
  setScheduleTarget,
  targetSet,
} from "../../www/fancoils/schedule-model.js";

const config = normalizeSchedulesConfiguration({
  version: 1,
  revision: 4,
  schedules: [
    {
      id: "workday-start",
      name: "Начало дня",
      enabled: true,
      panelId: "main",
      kind: "weekly",
      days: [5, 1, 3, 2, 4, 4],
      date: "",
      time: "08:00",
      targets: [{ bus: 2, address: 5 }, { bus: 1, address: 1 }],
      actions: { power: true, mode: 0, speed: 2, setTemp: 23 },
    },
    {
      id: "floor-2-off",
      name: "Выключение второго этажа",
      enabled: false,
      panelId: "floor-2",
      kind: "once",
      days: [],
      date: "2026-12-31",
      time: "18:30",
      targets: [{ bus: 3, address: 2 }],
      actions: { power: false },
    },
  ],
});
assert.equal(config.revision, 4);
assert.deepEqual(config.schedules[0].days, [1, 2, 3, 4, 5]);
assert.deepEqual(config.schedules[0].targets, [{ bus: 1, address: 1 }, { bus: 2, address: 5 }]);
assert.equal(schedulesForPanel(config, "main").length, 1);
assert.equal(schedulesForPanel(config, "floor-2").length, 1);
assert.equal(nextScheduleIdentifier(config), "schedule-1");
assert.equal(scheduleTargetKey(2, 18), "2:18");
assert.equal(scheduleTimingLabel(config.schedules[0]), "По будням, 08:00");
assert.equal(parseScheduleResultTopic("/mdvwb/schedules/workday-start/result"), "workday-start");
assert.equal(parseScheduleResultTopic("/mdvwb/schedules/bad/id/result"), null);

const copy = cloneSchedule(config.schedules[0]);
setScheduleTarget(copy, 1, 1, false);
setScheduleTarget(copy, 3, 2, true);
assert.deepEqual([...targetSet(copy)].sort(), ["2:5", "3:2"]);
assert.equal(config.schedules[0].targets.length, 2);

const created = createSchedule("main", config, new Date(2026, 6, 24, 10, 0));
assert.equal(created.id, "schedule-1");
assert.equal(created.time, "10:05");
assert.deepEqual(created.days, [1, 2, 3, 4, 5]);
assert.deepEqual(created.actions, { power: true });

const fridayBefore = new Date(2026, 6, 24, 7, 30);
const fridayRun = nextScheduleRun(config.schedules[0], fridayBefore);
assert.equal(fridayRun.getFullYear(), 2026);
assert.equal(fridayRun.getMonth(), 6);
assert.equal(fridayRun.getDate(), 24);
assert.equal(fridayRun.getHours(), 8);
assert.match(nextRunLabel(config.schedules[0], fridayBefore), /Следующий запуск:/);
assert.equal(nextScheduleRun(config.schedules[1], fridayBefore), null);

assert.throws(() => normalizeSchedule({ ...config.schedules[0], targets: [] }), /хотя бы один фанкойл/);
assert.throws(() => normalizeSchedule({ ...config.schedules[0], actions: {} }), /хотя бы один параметр/);
assert.throws(() => normalizeSchedule({ ...config.schedules[0], days: [] }), /дни недели/);
assert.throws(() => normalizeSchedule({ ...config.schedules[1], date: "2026-02-31" }), /несуществующая дата/);
assert.throws(() => normalizeSchedulesConfiguration({ ...config, schedules: [config.schedules[0], config.schedules[0]] }), /повторно/);
assert.equal(JSON.parse(schedulesConfigurationToJson(config)).revision, 4);

const html = readFileSync(new URL("../../www/fancoils/index.html", import.meta.url), "utf8");
const css = readFileSync(new URL("../../www/fancoils/styles.css", import.meta.url), "utf8");
const app = readFileSync(new URL("../../www/fancoils/app.js", import.meta.url), "utf8");
assert.match(html, /id="scheduleListView"/);
assert.match(html, /id="scheduleEditorView"/);
assert.match(html, /id="scheduleSelectAllButton"/);
assert.match(html, /data-schedule-day="7"/);
assert.match(html, /id="scheduleRunButton"/);
assert.doesNotMatch(html, /Расписание будет добавлено следующим этапом/);
assert.match(css, /fan-marker-schedule-selected/);
assert.match(app, /\/mdvwb\/schedules\/config\/set/);
assert.match(app, /\/mdvwb\/schedules\/\$\{state\.scheduleDraft\.id\}\/run/);
assert.match(app, /state\.client\.subscribe\("\/mdvwb\/scheduler\/status"\)/);

console.log("MDVWB schedules web model tests: OK");
