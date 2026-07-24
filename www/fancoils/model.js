import {
  dashboardAssetUrl,
  dashboardPanelToConfiguration,
  emptyDashboardConfiguration,
  findDashboardPanel,
  normalizeDashboardCollection,
  normalizeDashboardConfiguration,
  parseDashboardPayload,
} from "../mdvwb/dashboard-model.js";

export const FAN_CONTROLS = Object.freeze([
  "Power",
  "Mode",
  "Speed",
  "SetTemp",
  "Temp",
  "Blinds",
  "Blok",
  "Alarm",
  "AlarmCode",
  "Status",
]);

const CONTROL_SET = new Set(FAN_CONTROLS);

const STATUS_LABELS = Object.freeze({
  0: "Выключен",
  1: "Охлаждение",
  2: "Нагрев",
  3: "Осушение",
  4: "Вентиляция",
  5: "Автоматический режим",
  6: "Авария",
  7: "Нет связи",
});

const STATUS_CLASSES = Object.freeze({
  0: "off",
  1: "cooling",
  2: "heating",
  3: "dry",
  4: "fan",
  5: "auto",
  6: "alarm",
  7: "offline",
});

const MODE_LABELS = Object.freeze({
  0: "Охлаждение",
  1: "Нагрев",
  2: "Осушение",
  3: "Вентиляция",
  4: "Авто",
});

const SPEED_LABELS = Object.freeze({
  1: "Низкая",
  2: "Средняя",
  3: "Высокая",
  4: "Авто",
});

function finiteOrNull(value) {
  if (value === null || value === undefined || value === "") {
    return null;
  }
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

export function fanDeviceKey(bus, address) {
  return `${Number(bus)}:${Number(address)}`;
}

export function fanDeviceName(bus, address) {
  return `Fan-${Number(bus)}_${Number(address)}`;
}

export function parseFanControlTopic(topic) {
  const match = /^\/devices\/Fan-(\d{1,3})_(\d{1,2})\/controls\/([A-Za-z][A-Za-z0-9]*)$/.exec(String(topic));
  if (!match) {
    return null;
  }

  const bus = Number(match[1]);
  const address = Number(match[2]);
  const control = match[3];
  if (bus < 1 || bus > 999 || address < 0 || address > 63 || !CONTROL_SET.has(control)) {
    return null;
  }

  return {
    bus,
    address,
    control,
    key: fanDeviceKey(bus, address),
    device: fanDeviceName(bus, address),
  };
}

export function createFanState(bus, address) {
  return {
    bus: Number(bus),
    address: Number(address),
    Power: null,
    Mode: null,
    Speed: null,
    SetTemp: null,
    Temp: null,
    Blinds: null,
    Blok: null,
    Alarm: null,
    AlarmCode: null,
    Status: null,
    updatedAt: 0,
  };
}

export function applyFanControl(state, control, payload, now = Date.now()) {
  if (!CONTROL_SET.has(control)) {
    return state;
  }
  const next = { ...(state || createFanState(0, 0)) };
  next[control] = finiteOrNull(payload);
  next.updatedAt = Number(now) || Date.now();
  return next;
}

export function statusLabel(value) {
  const status = finiteOrNull(value);
  return status === null ? "Ожидание данных" : (STATUS_LABELS[status] || `Статус ${status}`);
}

export function statusClass(value) {
  const status = finiteOrNull(value);
  return status === null ? "unknown" : (STATUS_CLASSES[status] || "unknown");
}

export function modeLabel(value) {
  const mode = finiteOrNull(value);
  return mode === null ? "—" : (MODE_LABELS[mode] || String(mode));
}

export function speedLabel(value) {
  const speed = finiteOrNull(value);
  return speed === null ? "—" : (SPEED_LABELS[speed] || String(speed));
}

export function binaryLabel(value, enabledText = "Включено", disabledText = "Выключено") {
  const number = finiteOrNull(value);
  if (number === null) {
    return "—";
  }
  return number === 1 ? enabledText : disabledText;
}

export function temperatureLabel(value) {
  const number = finiteOrNull(value);
  if (number === null) {
    return "—";
  }
  return `${Number.isInteger(number) ? number.toFixed(0) : number.toFixed(1)} °C`;
}

export function alarmLabel(state) {
  const alarm = finiteOrNull(state?.Alarm);
  const code = finiteOrNull(state?.AlarmCode);
  if (alarm === null) {
    return "Ожидание данных";
  }
  if (alarm === 0) {
    return "Нет аварии";
  }
  if (alarm === 2) {
    return "Нет связи";
  }
  return code && code > 0 ? `Авария, код ${code}` : "Авария фанкойла";
}

export function isOfflineState(state) {
  return Number(state?.Status) === 7 || Number(state?.Alarm) === 2;
}

export function isAlarmState(state) {
  return Number(state?.Status) === 6 || Number(state?.Alarm) === 1;
}

export function hasFanData(state) {
  return state?.Status !== null && state?.Status !== undefined;
}

export function summarizeDashboard(dashboardInput, states) {
  const dashboard = normalizeDashboardConfiguration(dashboardInput || emptyDashboardConfiguration());
  const stateMap = states instanceof Map ? states : new Map(Object.entries(states || {}));
  const visibleFans = dashboard.fans.filter((fan) => fan.visible);
  let online = 0;
  let alarms = 0;
  let offline = 0;
  let waiting = 0;

  visibleFans.forEach((fan) => {
    const state = stateMap.get(fanDeviceKey(fan.bus, fan.address));
    if (!hasFanData(state)) {
      waiting += 1;
    } else if (isOfflineState(state)) {
      offline += 1;
    } else {
      online += 1;
      if (isAlarmState(state)) {
        alarms += 1;
      }
    }
  });

  return {
    placed: dashboard.fans.length,
    visible: visibleFans.length,
    online,
    alarms,
    offline,
    waiting,
  };
}

export function markerMatchesFilter(state, filter) {
  switch (String(filter || "all")) {
    case "alarm":
      return isAlarmState(state);
    case "offline":
      return isOfflineState(state);
    case "online":
      return hasFanData(state) && !isOfflineState(state);
    case "waiting":
      return !hasFanData(state);
    default:
      return true;
  }
}

export function clampScale(value) {
  return Math.min(4, Math.max(0.1, Number(value) || 1));
}

export function computeFitScale(fit, naturalWidth, naturalHeight, viewportWidth, viewportHeight, defaultScale = 1) {
  const width = Math.max(1, Number(naturalWidth) || 1600);
  const height = Math.max(1, Number(naturalHeight) || 900);
  const availableWidth = Math.max(160, (Number(viewportWidth) || width) - 32);
  const availableHeight = Math.max(120, (Number(viewportHeight) || height) - 32);

  switch (fit) {
    case "width":
      return clampScale(availableWidth / width);
    case "actual":
      return 1;
    case "custom":
      return clampScale(defaultScale);
    case "contain":
    default:
      return clampScale(Math.min(availableWidth / width, availableHeight / height));
  }
}

export function dashboardSelectionFromPayload(payload, requestedPanelId = "") {
  const collection = normalizeDashboardCollection(
    parseDashboardPayload(payload, "конфигурацию веб-панелей"),
  );
  const requested = String(requestedPanelId || "").trim();
  const selectedId = requested && findDashboardPanel(collection, requested)
    ? requested
    : collection.defaultPanel;
  const panel = findDashboardPanel(collection, selectedId);
  if (!panel) {
    throw new Error("В конфигурации отсутствует панель по умолчанию");
  }
  return {
    collection,
    panelId: selectedId,
    requestedFound: !requested || requested === selectedId,
    dashboard: dashboardPanelToConfiguration(panel, collection.revision),
  };
}

export function dashboardFromPayload(payload, requestedPanelId = "") {
  return dashboardSelectionFromPayload(payload, requestedPanelId).dashboard;
}

export function backgroundUrl(dashboard) {
  return dashboardAssetUrl(dashboard?.background?.file || "");
}

export const FAN_COMMAND_CONTROLS = Object.freeze([
  "Power",
  "Mode",
  "Speed",
  "SetTemp",
  "Blinds",
  "Blok",
]);

const COMMAND_CONTROL_SET = new Set(FAN_COMMAND_CONTROLS);

export function fanCommandTopic(bus, address, control) {
  const busNumber = Number(bus);
  const addressNumber = Number(address);
  const name = String(control);
  if (!Number.isInteger(busNumber) || busNumber < 1 || busNumber > 999) {
    throw new Error("Неверный номер шины");
  }
  if (!Number.isInteger(addressNumber) || addressNumber < 0 || addressNumber > 63) {
    throw new Error("Неверный адрес фанкойла");
  }
  if (!COMMAND_CONTROL_SET.has(name)) {
    throw new Error(`Параметр ${name} нельзя изменять`);
  }
  return `/devices/${fanDeviceName(busNumber, addressNumber)}/controls/${name}/on1`;
}

export function normalizeFanCommand(control, value) {
  const name = String(control);
  if (!COMMAND_CONTROL_SET.has(name)) {
    throw new Error(`Параметр ${name} нельзя изменять`);
  }
  const number = Number(value);
  if (!Number.isInteger(number)) {
    throw new Error("Команда должна содержать целое число");
  }

  if ((name === "Power" || name === "Blinds" || name === "Blok") && (number === 0 || number === 1)) {
    return number;
  }
  if (name === "Mode" && number >= 0 && number <= 4) {
    return number;
  }
  if (name === "Speed" && number >= 1 && number <= 4) {
    return number;
  }
  if (name === "SetTemp" && number >= 16 && number <= 32) {
    return number;
  }

  const ranges = {
    Power: "0 или 1",
    Mode: "0..4",
    Speed: "1..4",
    SetTemp: "16..32",
    Blinds: "0 или 1",
    Blok: "0 или 1",
  };
  throw new Error(`Недопустимое значение ${name}: ожидается ${ranges[name]}`);
}

export function fanCommandMatchesState(control, expectedValue, fanState) {
  if (!COMMAND_CONTROL_SET.has(String(control)) || !fanState) {
    return false;
  }
  return Number(fanState[control]) === Number(expectedValue);
}

export function canSendFanCommands(connected, fanState) {
  return Boolean(connected) && hasFanData(fanState) && !isOfflineState(fanState);
}



export const GROUP_COMMAND_CONTROLS = Object.freeze([
  "Power",
  "Mode",
  "Speed",
  "SetTemp",
]);

export function normalizeGroupCommands(input) {
  const source = input && typeof input === "object" ? input : {};
  const commands = [];
  GROUP_COMMAND_CONTROLS.forEach((control) => {
    const entry = source[control];
    if (!entry || entry.enabled !== true) {
      return;
    }
    commands.push({ control, value: normalizeFanCommand(control, entry.value) });
  });
  if (!commands.length) {
    throw new Error("Выберите хотя бы один параметр групповой команды");
  }
  return commands;
}

export function buildGroupCommandPlan(
  dashboardInput,
  selectedKeysInput,
  statesInput,
  connected,
  commandInput,
  pendingKeysInput = [],
) {
  const dashboard = normalizeDashboardConfiguration(dashboardInput || emptyDashboardConfiguration());
  const selectedKeys = selectedKeysInput instanceof Set
    ? selectedKeysInput
    : new Set(Array.isArray(selectedKeysInput) ? selectedKeysInput : []);
  const states = statesInput instanceof Map ? statesInput : new Map(Object.entries(statesInput || {}));
  const pendingKeys = pendingKeysInput instanceof Set
    ? pendingKeysInput
    : new Set(Array.isArray(pendingKeysInput) ? pendingKeysInput : []);
  const commands = normalizeGroupCommands(commandInput);
  const targets = [];
  const skipped = [];
  const operations = [];
  const pendingSkipped = [];

  dashboard.fans.forEach((fan) => {
    const key = fanDeviceKey(fan.bus, fan.address);
    if (!selectedKeys.has(key)) {
      return;
    }
    const fanState = states.get(key) || createFanState(fan.bus, fan.address);
    if (!connected) {
      skipped.push({ fan, key, reason: "mqtt" });
      return;
    }
    if (!hasFanData(fanState)) {
      skipped.push({ fan, key, reason: "waiting" });
      return;
    }
    if (isOfflineState(fanState)) {
      skipped.push({ fan, key, reason: "offline" });
      return;
    }

    targets.push({ fan, key, state: fanState });
    commands.forEach(({ control, value }) => {
      const pending = `${key}:${control}`;
      if (pendingKeys.has(pending)) {
        pendingSkipped.push({ fan, key, control, reason: "pending" });
        return;
      }
      operations.push({
        fan,
        key,
        control,
        value,
        topic: fanCommandTopic(fan.bus, fan.address, control),
      });
    });
  });

  return { commands, targets, skipped, pendingSkipped, operations };
}
