import assert from "node:assert/strict";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

import {
  busFromEditorValues,
  canRunBusCommand,
  cloneConfiguration,
  configurationToJson,
  findModbusProfile,
  normalizeBus,
  normalizeConfiguration,
  normalizeModbusProfileCatalog,
} from "../../www/mdvwb/model.js";
import {
  describeModbusCapabilities,
  protocolDisplayName,
} from "../../www/mdvwb/modbus-profile-ui.js";

const repositoryRoot = path.resolve(
  path.dirname(fileURLToPath(import.meta.url)),
  "../..",
);

function expectThrows(callback, text) {
  assert.throws(callback, (error) =>
    error instanceof Error && error.message.includes(text));
}

function productionCatalog() {
  return normalizeModbusProfileCatalog({
    schemaVersion: 1,
    profiles: [{
      id: "vrf_add_controller",
      name: "VRF Add Controller",
      transport: {
        baudRate: 9600,
        dataBits: 8,
        parity: "none",
        stopBits: 1,
      },
      logicalAddresses: {
        minimum: 1,
        maximum: 63,
      },
      addressingType: "fixed_slave_stride",
      capabilities: {
        power: {
          supported: true,
          readable: true,
          writable: true,
          type: "boolean",
        },
        mode: {
          supported: false,
          readable: false,
          writable: false,
          type: null,
        },
        fanSpeed: {
          supported: false,
          readable: false,
          writable: false,
          type: null,
        },
        setTemperature: {
          supported: false,
          readable: false,
          writable: false,
          type: null,
        },
        roomTemperature: {
          supported: false,
          readable: false,
          writable: false,
          type: null,
        },
        alarm: {
          supported: true,
          readable: true,
          writable: false,
          type: "number",
          minimum: null,
          maximum: null,
          step: null,
        },
        blinds: {
          supported: false,
          readable: false,
          writable: false,
          type: null,
        },
        blocked: {
          supported: false,
          readable: false,
          writable: false,
          type: null,
        },
      },
    }],
    issues: [],
  });
}

function testLegacyMdvCompatibility() {
  const bus = normalizeBus({
    id: 1,
    enabled: true,
    port: "/dev/ttyRS485-1",
    addresses: [0, 1],
  });
  assert.equal(bus.protocol, "mdv");
  assert.equal("modbus" in bus, false);
  assert.deepEqual(bus.addresses, [0, 1]);
}

function testModbusBusIsDerivedFromCatalog() {
  const catalog = productionCatalog();
  const bus = busFromEditorValues({
    id: "2",
    enabled: true,
    protocol: "modbus_rtu",
    profileId: "vrf_add_controller",
    port: "/dev/ttyRS485-2",
    addresses: "3, 1, 2",
  }, catalog);

  assert.deepEqual(bus, {
    id: 2,
    enabled: true,
    protocol: "modbus_rtu",
    port: "/dev/ttyRS485-2",
    modbus: {
      profileId: "vrf_add_controller",
      baudRate: 9600,
      dataBits: 8,
      parity: "none",
      stopBits: 1,
    },
    addresses: [1, 2, 3],
  });
}

function testProtocolAddressBoundaries() {
  expectThrows(() => normalizeBus({
    id: 2,
    enabled: true,
    protocol: "modbus_rtu",
    port: "/dev/ttyRS485-2",
    modbus: {
      profileId: "vrf_add_controller",
      baudRate: 9600,
      dataBits: 8,
      parity: "none",
      stopBits: 1,
    },
    addresses: [0],
  }), "1–63");

  expectThrows(() => busFromEditorValues({
    id: 2,
    enabled: true,
    protocol: "modbus_rtu",
    profileId: "missing",
    port: "/dev/ttyRS485-2",
    addresses: "1",
  }, productionCatalog()), "недоступен");
}

function testProfileSpecificAddressRange() {
  const catalog = productionCatalog();
  catalog.profiles[0].logicalAddresses.maximum = 2;

  expectThrows(() => busFromEditorValues({
    id: 2,
    enabled: true,
    protocol: "modbus_rtu",
    profileId: "vrf_add_controller",
    port: "/dev/ttyRS485-2",
    addresses: "3",
  }, catalog), "1–2");
}

function testCanonicalConfigurationAndClone() {
  const config = normalizeConfiguration({
    version: 1,
    revision: 7,
    buses: [
      {
        id: 2,
        enabled: true,
        protocol: "modbus_rtu",
        port: "/dev/ttyRS485-2",
        modbus: {
          profileId: "vrf_add_controller",
          baudRate: 9600,
          dataBits: 8,
          parity: "none",
          stopBits: 1,
        },
        addresses: [2, 1],
      },
      {
        id: 1,
        enabled: false,
        port: "/dev/ttyRS485-1",
        addresses: [],
      },
    ],
  });

  const serialized = configurationToJson(config);
  const parsed = JSON.parse(serialized);
  assert.equal(parsed.buses[0].protocol, "mdv");
  assert.equal("modbus" in parsed.buses[0], false);
  assert.equal(parsed.buses[1].modbus.profileId, "vrf_add_controller");

  const clone = cloneConfiguration(config);
  clone.buses[1].modbus.baudRate = 19200;
  clone.buses[1].addresses.push(3);
  assert.equal(config.buses[1].modbus.baudRate, 9600);
  assert.deepEqual(config.buses[1].addresses, [1, 2]);
}

function testCatalogAndCapabilities() {
  const catalog = productionCatalog();
  const profile = findModbusProfile(catalog, "vrf_add_controller");
  assert.ok(profile);
  assert.equal(profile.transport.baudRate, 9600);
  assert.equal(
    describeModbusCapabilities(profile),
    "Power: чтение и запись · Alarm: только чтение",
  );
  assert.equal(
    protocolDisplayName({
      protocol: "modbus_rtu",
      modbus: { profileId: "vrf_add_controller" },
    }, catalog),
    "Modbus RTU · VRF Add Controller",
  );
  assert.equal(protocolDisplayName({ protocol: "mdv" }, catalog), "MDV");
}

function testCatalogValidation() {
  const catalog = productionCatalog();
  catalog.profiles.push(structuredClone(catalog.profiles[0]));
  expectThrows(
    () => normalizeModbusProfileCatalog(catalog),
    "повторно",
  );
}


function testModbusDiscoveryStaysDisabledUntilStepFour() {
  assert.equal(canRunBusCommand({
    command: "discovery",
    enabled: true,
    connected: true,
    demo: false,
    dirty: false,
    pending: false,
    discoveryRunning: false,
    protocol: "modbus_rtu",
  }), false);
  assert.equal(canRunBusCommand({
    command: "discovery",
    enabled: true,
    connected: true,
    demo: false,
    dirty: false,
    pending: false,
    discoveryRunning: false,
    protocol: "mdv",
  }), true);
}

function testApplicationWiring() {
  const app = fs.readFileSync(
    path.join(repositoryRoot, "www/mdvwb/app.js"),
    "utf8",
  );
  assert.match(app, /subscribe\("\/mdvwb\/modbus\/profiles"\)/);
  assert.match(app, /topic === "\/mdvwb\/modbus\/profiles"/);
  assert.match(app, /new ModbusBusEditor/);
  assert.doesNotMatch(app, /if\s*\([^)]*profileId\s*===\s*["']/);
}

testLegacyMdvCompatibility();
testModbusBusIsDerivedFromCatalog();
testProtocolAddressBoundaries();
testProfileSpecificAddressRange();
testCanonicalConfigurationAndClone();
testCatalogAndCapabilities();
testCatalogValidation();
testModbusDiscoveryStaysDisabledUntilStepFour();
testApplicationWiring();

console.log("MDVWB Modbus web editor model tests: OK");
