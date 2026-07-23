import assert from "node:assert/strict";
import fs from "node:fs/promises";

const source = await fs.readFile(new URL("../www/mdvwb/model.js", import.meta.url), "utf8");
const moduleUrl = `data:text/javascript;base64,${Buffer.from(source).toString("base64")}`;
const model = await import(moduleUrl);

assert.deepEqual(model.parseAddressesInput("3, 1, 2"), [3, 1, 2]);
assert.deepEqual(model.busFromEditorValues({
  id: "2",
  enabled: true,
  port: " /dev/ttyUSB0 ",
  addresses: "3, 1, 2",
}), {
  id: 2,
  enabled: true,
  port: "/dev/ttyUSB0",
  addresses: [1, 2, 3],
});

assert.throws(
  () => model.busFromEditorValues({ id: 1, enabled: true, port: "/dev/ttyUSB0", addresses: "" }),
  /хотя бы один адрес/,
);
assert.throws(
  () => model.busFromEditorValues({ id: 1, enabled: false, port: "ttyUSB0", addresses: "" }),
  /начинающимся с \/dev\//,
);
assert.throws(
  () => model.busFromEditorValues({ id: 1, enabled: true, port: "/dev/ttyUSB0", addresses: "1,1" }),
  /повторно/,
);
assert.throws(
  () => model.busFromEditorValues({ id: 1, enabled: true, port: "/dev/ttyUSB0", addresses: "64" }),
  /0–63/,
);

const config = model.normalizeConfiguration({
  version: 1,
  buses: [
    { id: 3, enabled: false, port: "/dev/ttyUSB2", addresses: [] },
    { id: 1, enabled: true, port: "/dev/ttyUSB0", addresses: [3, 1, 2] },
  ],
});
assert.deepEqual(config.buses.map((bus) => bus.id), [1, 3]);
assert.deepEqual(config.buses[0].addresses, [1, 2, 3]);
assert.equal(model.nextAvailableBusId(config), 2);
assert.equal(model.configurationsEqual(config, model.cloneConfiguration(config)), true);

assert.throws(
  () => model.normalizeConfiguration({
    version: 1,
    buses: [
      { id: 1, enabled: true, port: "/dev/ttyUSB0", addresses: [1] },
      { id: 1, enabled: true, port: "/dev/ttyUSB1", addresses: [2] },
    ],
  }),
  /указан повторно/,
);
assert.throws(
  () => model.normalizeConfiguration({
    version: 1,
    buses: [
      { id: 1, enabled: true, port: "/dev/ttyUSB0", addresses: [1] },
      { id: 2, enabled: true, port: "/dev/ttyUSB0", addresses: [2] },
    ],
  }),
  /назначен нескольким/,
);

assert.equal(model.busCommandTopic(3, "start"), "/mdvwb/buses/3/start");
assert.equal(model.busCommandTopic(3, "stop"), "/mdvwb/buses/3/stop");
assert.equal(model.busCommandTopic(3, "restart"), "/mdvwb/buses/3/restart");
assert.equal(model.busCommandTopic(3, "status"), "/mdvwb/buses/3/status/get");
assert.equal(model.busCommandTopic(3, "discovery"), "/mdvwb/buses/3/discovery/start");
assert.throws(() => model.busCommandTopic(0, "start"), /1–999/);
assert.throws(() => model.busCommandTopic(1, "remove"), /Неизвестная команд/);

assert.deepEqual(model.parseBusTopic("/mdvwb/buses/18/status"), { busId: 18, kind: "status" });
assert.deepEqual(model.parseBusTopic("/mdvwb/buses/18/result"), { busId: 18, kind: "result" });
assert.deepEqual(model.parseBusTopic("/mdvwb/buses/18/discovery/status"), {
  busId: 18,
  kind: "discovery/status",
});
assert.deepEqual(model.parseBusTopic("/mdvwb/buses/18/discovery/result"), {
  busId: 18,
  kind: "discovery/result",
});
assert.equal(model.parseBusTopic("/mdvwb/buses/x/status"), null);

const available = {
  enabled: true,
  connected: true,
  demo: false,
  dirty: false,
  pending: false,
  discoveryRunning: false,
};
assert.equal(model.canRunBusCommand({ ...available, command: "start" }), true);
assert.equal(model.canRunBusCommand({ ...available, command: "restart" }), true);
assert.equal(model.canRunBusCommand({ ...available, command: "discovery" }), true);
assert.equal(model.canRunBusCommand({ ...available, command: "start", enabled: false }), false);
assert.equal(model.canRunBusCommand({ ...available, command: "stop", enabled: false }), true);
assert.equal(model.canRunBusCommand({ ...available, command: "status", dirty: true }), false);
assert.equal(model.canRunBusCommand({ ...available, command: "stop", pending: true }), false);
assert.equal(model.canRunBusCommand({ ...available, command: "stop", discoveryRunning: true }), false);

assert.equal(model.discoveryLabel("error"), "Ошибка поиска");
assert.equal(model.commandLabel("restart"), "Перезапуск");

console.log("MDVWB web control model tests: OK");
