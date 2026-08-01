const DEVICE_PATH_PATTERN = /^\/dev\/[A-Za-z0-9/_+.:-]+$/;
const PROFILE_ID_PATTERN = /^[a-z][a-z0-9_]{0,63}$/;
const BUS_COMMANDS = new Set(["start", "stop", "restart", "status", "discovery"]);
const BUS_PROTOCOLS = new Set(["mdv", "modbus_rtu"]);
const BUS_PARITIES = new Set(["none", "even", "odd"]);
const MODBUS_BAUD_RATES = new Set([1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200]);
const CAPABILITY_NAMES = [
  "power",
  "mode",
  "fanSpeed",
  "setTemperature",
  "roomTemperature",
  "alarm",
  "blinds",
  "blocked",
];
let latestConfigurationRevision = 0;

function requireInteger(value, minimum, maximum, description) {
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    throw new Error(`${description}: допустимо целое число ${minimum}–${maximum}`);
  }
  return value;
}

function requireObject(value, description) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${description}: ожидался объект`);
  }
  return value;
}

function requireString(value, description) {
  if (typeof value !== "string" || value.trim() === "") {
    throw new Error(`${description}: ожидалась непустая строка`);
  }
  return value.trim();
}

function normalizePort(value) {
  const port = String(value ?? "").trim();
  if (port.length <= 5 || !DEVICE_PATH_PATTERN.test(port)) {
    throw new Error("Порт должен быть безопасным путём устройства, начинающимся с /dev/");
  }
  return port;
}

function normalizeProtocol(value) {
  const protocol = value === undefined ? "mdv" : String(value);
  if (!BUS_PROTOCOLS.has(protocol)) {
    throw new Error("Протокол шины должен быть mdv или modbus_rtu");
  }
  return protocol;
}

function normalizeAddresses(values, enabled, minimum = 0, maximum = 63) {
  if (!Array.isArray(values)) {
    throw new Error("Список адресов должен быть массивом");
  }

  const unique = new Set();
  const addresses = values.map((value) => {
    const address = requireInteger(value, minimum, maximum, "Адрес фанкойла");
    if (unique.has(address)) {
      throw new Error(`Адрес ${address} указан повторно`);
    }
    unique.add(address);
    return address;
  });

  addresses.sort((left, right) => left - right);
  if (enabled && addresses.length === 0) {
    throw new Error("Для активной шины нужно указать хотя бы один адрес");
  }
  return addresses;
}

function normalizeModbusSettings(value) {
  const settings = requireObject(value, "Настройки Modbus");
  const profileId = requireString(settings.profileId, "ID профиля");
  if (!PROFILE_ID_PATTERN.test(profileId)) {
    throw new Error("ID профиля должен начинаться с a–z и содержать только a–z, 0–9 и _");
  }

  const baudRate = requireInteger(settings.baudRate, 1, 2147483647, "Скорость Modbus");
  if (!MODBUS_BAUD_RATES.has(baudRate)) {
    throw new Error("Скорость Modbus должна быть одной из поддерживаемых стандартных скоростей");
  }

  const dataBits = requireInteger(settings.dataBits, 7, 8, "Биты данных Modbus");
  const parity = String(settings.parity ?? "");
  if (!BUS_PARITIES.has(parity)) {
    throw new Error("Чётность Modbus должна быть none, even или odd");
  }
  const stopBits = requireInteger(settings.stopBits, 1, 2, "Стоп-биты Modbus");

  return { profileId, baudRate, dataBits, parity, stopBits };
}

function normalizeTransport(value) {
  const transport = requireObject(value, "Настройки транспорта профиля");
  const baudRate = requireInteger(transport.baudRate, 1, 2147483647, "Скорость профиля");
  if (!MODBUS_BAUD_RATES.has(baudRate)) {
    throw new Error("Профиль содержит неподдерживаемую скорость Modbus");
  }
  const dataBits = requireInteger(transport.dataBits, 7, 8, "Биты данных профиля");
  const parity = String(transport.parity ?? "");
  if (!BUS_PARITIES.has(parity)) {
    throw new Error("Профиль содержит неподдерживаемую чётность");
  }
  const stopBits = requireInteger(transport.stopBits, 1, 2, "Стоп-биты профиля");
  return { baudRate, dataBits, parity, stopBits };
}

function normalizeLogicalRange(value) {
  const range = requireObject(value, "Диапазон логических адресов профиля");
  const minimum = requireInteger(range.minimum, 1, 63, "Минимальный логический адрес");
  const maximum = requireInteger(range.maximum, 1, 63, "Максимальный логический адрес");
  if (minimum > maximum) {
    throw new Error("Минимальный логический адрес профиля больше максимального");
  }
  return { minimum, maximum };
}

function normalizeCapability(value, name) {
  const capability = requireObject(value, `Возможность ${name}`);
  const supported = capability.supported === true;
  const readable = supported && capability.readable === true;
  const writable = supported && capability.writable === true;
  const type = capability.type === null || capability.type === undefined
    ? null
    : String(capability.type);
  if (type !== null && type !== "boolean" && type !== "enum" && type !== "number") {
    throw new Error(`Возможность ${name} содержит неизвестный тип`);
  }

  const result = { supported, readable, writable, type };
  if (type === "enum") {
    const values = Array.isArray(capability.values) ? capability.values : [];
    result.values = values.map((entry) => {
      const item = requireObject(entry, `Значение возможности ${name}`);
      return {
        value: requireString(item.value, `Значение возможности ${name}`),
        readable: item.readable === true,
        writable: item.writable === true,
      };
    }).sort((left, right) => left.value.localeCompare(right.value));
  }
  if (type === "number") {
    for (const field of ["minimum", "maximum", "step"]) {
      const numeric = capability[field];
      result[field] = numeric === null || numeric === undefined ? null : Number(numeric);
      if (result[field] !== null && !Number.isFinite(result[field])) {
        throw new Error(`Возможность ${name} содержит некорректное поле ${field}`);
      }
    }
  }
  return result;
}

function normalizeProfile(value) {
  const profile = requireObject(value, "Профиль Modbus");
  const id = requireString(profile.id, "ID профиля");
  if (!PROFILE_ID_PATTERN.test(id)) {
    throw new Error("Каталог содержит некорректный ID профиля");
  }

  const capabilitiesSource = requireObject(profile.capabilities, `Возможности профиля ${id}`);
  const capabilities = {};
  CAPABILITY_NAMES.forEach((name) => {
    capabilities[name] = normalizeCapability(
      capabilitiesSource[name] ?? {
        supported: false,
        readable: false,
        writable: false,
        type: null,
      },
      name,
    );
  });

  return {
    id,
    name: requireString(profile.name, `Название профиля ${id}`),
    transport: normalizeTransport(profile.transport),
    logicalAddresses: normalizeLogicalRange(profile.logicalAddresses),
    addressingType: requireString(profile.addressingType, `Тип адресации профиля ${id}`),
    capabilities,
  };
}

export function normalizeModbusProfileCatalog(value) {
  const catalog = requireObject(value, "Каталог Modbus-профилей");
  if (catalog.schemaVersion !== 1 || !Array.isArray(catalog.profiles) || !Array.isArray(catalog.issues)) {
    throw new Error("Некорректный каталог Modbus-профилей");
  }

  const profiles = catalog.profiles.map(normalizeProfile);
  const ids = new Set();
  profiles.forEach((profile) => {
    if (ids.has(profile.id)) {
      throw new Error(`Профиль ${profile.id} указан в каталоге повторно`);
    }
    ids.add(profile.id);
  });
  profiles.sort((left, right) => left.id.localeCompare(right.id));

  const issues = catalog.issues.map((value) => {
    const issue = requireObject(value, "Ошибка каталога профилей");
    return {
      file: requireString(issue.file, "Файл ошибки каталога"),
      message: requireString(issue.message, "Сообщение ошибки каталога"),
    };
  }).sort((left, right) =>
    left.file.localeCompare(right.file) || left.message.localeCompare(right.message));

  return { schemaVersion: 1, profiles, issues };
}

export function findModbusProfile(catalog, profileId) {
  const normalized = normalizeModbusProfileCatalog(catalog);
  return normalized.profiles.find((profile) => profile.id === profileId) ?? null;
}

export function modbusSettingsFromProfile(profile) {
  const normalized = normalizeProfile(profile);
  return {
    profileId: normalized.id,
    baudRate: normalized.transport.baudRate,
    dataBits: normalized.transport.dataBits,
    parity: normalized.transport.parity,
    stopBits: normalized.transport.stopBits,
  };
}

export function parseAddressesInput(value) {
  const text = String(value ?? "").trim();
  if (text === "") {
    return [];
  }

  return text.split(",").map((part) => {
    const token = part.trim();
    if (!/^\d+$/.test(token)) {
      throw new Error(`Некорректный адрес «${token || "пусто"}»`);
    }
    return Number(token);
  });
}

export function normalizeBus(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error("Некорректное описание шины");
  }
  if (typeof value.enabled !== "boolean") {
    throw new Error("Признак активности шины должен быть true или false");
  }

  const enabled = value.enabled;
  const protocol = normalizeProtocol(value.protocol);
  const result = {
    id: requireInteger(value.id, 1, 999, "Номер шины"),
    enabled,
    protocol,
    port: normalizePort(value.port),
  };

  if (protocol === "modbus_rtu") {
    if (value.modbus === undefined) {
      throw new Error("Для Modbus RTU нужно выбрать профиль");
    }
    result.modbus = normalizeModbusSettings(value.modbus);
  } else if (value.modbus !== undefined) {
    throw new Error("Настройки Modbus допустимы только для протокола modbus_rtu");
  }

  result.addresses = normalizeAddresses(
    value.addresses,
    enabled,
    protocol === "modbus_rtu" ? 1 : 0,
    63,
  );
  return result;
}

export function normalizeConfiguration(value) {
  if (!value || value.version !== 1 || !Array.isArray(value.buses)) {
    throw new Error("Некорректная конфигурация шин");
  }

  const hasExplicitRevision = value.revision !== undefined;
  const revision = hasExplicitRevision
    ? requireInteger(value.revision, 0, 2147483647, "Ревизия")
    : latestConfigurationRevision;
  if (hasExplicitRevision) {
    latestConfigurationRevision = Math.max(latestConfigurationRevision, revision);
  }
  const buses = value.buses.map(normalizeBus);
  const ids = new Set();
  const ports = new Set();

  buses.forEach((bus) => {
    if (ids.has(bus.id)) {
      throw new Error(`Номер шины ${bus.id} указан повторно`);
    }
    if (ports.has(bus.port)) {
      throw new Error(`Порт ${bus.port} назначен нескольким шинам`);
    }
    ids.add(bus.id);
    ports.add(bus.port);
  });

  buses.sort((left, right) => left.id - right.id);
  return { version: 1, revision, buses };
}

export function busFromEditorValues(
  { id, enabled, protocol = "mdv", port, addresses, profileId },
  profileCatalog = { schemaVersion: 1, profiles: [], issues: [] },
) {
  const numericId = typeof id === "number" ? id : Number(String(id).trim());
  const normalizedProtocol = normalizeProtocol(protocol);
  const value = {
    id: numericId,
    enabled: enabled === true,
    protocol: normalizedProtocol,
    port,
    addresses: parseAddressesInput(addresses),
  };

  if (normalizedProtocol === "modbus_rtu") {
    const selectedProfileId = String(profileId ?? "").trim();
    const profile = findModbusProfile(profileCatalog, selectedProfileId);
    if (!profile) {
      throw new Error("Выбранный Modbus-профиль недоступен");
    }
    value.modbus = modbusSettingsFromProfile(profile);
    value.addresses.forEach((address) => {
      if (address < profile.logicalAddresses.minimum ||
          address > profile.logicalAddresses.maximum) {
        throw new Error(
          `Адрес фанкойла: допустимо целое число ${profile.logicalAddresses.minimum}–${profile.logicalAddresses.maximum}`,
        );
      }
    });
  }

  return normalizeBus(value);
}

export function cloneConfiguration(value) {
  const normalized = normalizeConfiguration(value);
  return {
    version: 1,
    revision: normalized.revision,
    buses: normalized.buses.map((bus) => ({
      ...bus,
      ...(bus.modbus ? { modbus: { ...bus.modbus } } : {}),
      addresses: [...bus.addresses],
    })),
  };
}

export function configurationsEqual(left, right) {
  return configurationToJson(left) === configurationToJson(right);
}

export function configurationToJson(value) {
  return `${JSON.stringify(normalizeConfiguration(value), null, 2)}\n`;
}

export function nextAvailableBusId(configuration) {
  const normalized = normalizeConfiguration(configuration);
  const used = new Set(normalized.buses.map((bus) => bus.id));
  for (let id = 1; id <= 999; id += 1) {
    if (!used.has(id)) {
      return id;
    }
  }
  throw new Error("Свободных номеров шин больше нет");
}

export function parseJsonPayload(payload, description) {
  try {
    return JSON.parse(payload);
  } catch (_error) {
    throw new Error(`Получен некорректный JSON: ${description}`);
  }
}

export function busCommandTopic(busId, command) {
  const id = requireInteger(Number(busId), 1, 999, "Номер шины");
  if (!BUS_COMMANDS.has(command)) {
    throw new Error(`Неизвестная команда шины: ${command}`);
  }

  const suffixes = {
    start: "start",
    stop: "stop",
    restart: "restart",
    status: "status/get",
    discovery: "discovery/start",
  };
  return `/mdvwb/buses/${id}/${suffixes[command]}`;
}

export function parseBusTopic(topic) {
  const match = String(topic).match(
    /^\/mdvwb\/buses\/(\d+)\/(status|result|discovery\/status|discovery\/result)$/,
  );
  if (!match) {
    return null;
  }
  const busId = Number(match[1]);
  if (!Number.isInteger(busId) || busId < 1 || busId > 999) {
    return null;
  }
  return { busId, kind: match[2] };
}

export function canRunBusCommand({
  command,
  enabled,
  connected,
  demo,
  dirty,
  pending,
  discoveryRunning,
  protocol = "mdv",
}) {
  if (!BUS_COMMANDS.has(command)) {
    return false;
  }
  if ((!connected && !demo) || dirty || pending || discoveryRunning) {
    return false;
  }
  if ((command === "start" || command === "restart") && !enabled) {
    return false;
  }
  if (command === "discovery" && protocol === "modbus_rtu") {
    return false;
  }
  return true;
}

export function serviceLabel(service) {
  const value = String(service || "unknown").toLowerCase();
  const labels = {
    active: "Работает",
    inactive: "Остановлена",
    failed: "Ошибка",
    activating: "Запускается",
    deactivating: "Останавливается",
    unknown: "Нет данных",
  };
  return labels[value] || value;
}

export function discoveryLabel(state) {
  const value = String(state || "idle").toLowerCase();
  const labels = {
    running: "Поиск выполняется…",
    completed: "Поиск завершён",
    error: "Ошибка поиска",
    failed: "Ошибка поиска",
    idle: "Поиск ещё не запускался",
  };
  return labels[value] || value;
}

export function commandLabel(command) {
  const labels = {
    start: "Запуск",
    stop: "Остановка",
    restart: "Перезапуск",
    status: "Обновление состояния",
    discovery: "Поиск устройств",
  };
  return labels[command] || command;
}

export function formatAddresses(addresses) {
  return Array.isArray(addresses) && addresses.length > 0 ? addresses.join(", ") : "—";
}
