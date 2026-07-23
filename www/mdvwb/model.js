const DEVICE_PATH_PATTERN = /^\/dev\/[A-Za-z0-9/_+.:-]+$/;
const BUS_COMMANDS = new Set(["start", "stop", "restart", "status", "discovery"]);

function requireInteger(value, minimum, maximum, description) {
  if (!Number.isInteger(value) || value < minimum || value > maximum) {
    throw new Error(`${description}: допустимо целое число ${minimum}–${maximum}`);
  }
  return value;
}

function normalizePort(value) {
  const port = String(value ?? "").trim();
  if (port.length <= 5 || !DEVICE_PATH_PATTERN.test(port)) {
    throw new Error("Порт должен быть безопасным путём устройства, начинающимся с /dev/");
  }
  return port;
}

function normalizeAddresses(values, enabled) {
  if (!Array.isArray(values)) {
    throw new Error("Список адресов должен быть массивом");
  }

  const unique = new Set();
  const addresses = values.map((value) => {
    const address = requireInteger(value, 0, 63, "Адрес фанкойла");
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
  return {
    id: requireInteger(value.id, 1, 999, "Номер шины"),
    enabled,
    port: normalizePort(value.port),
    addresses: normalizeAddresses(value.addresses, enabled),
  };
}

export function normalizeConfiguration(value) {
  if (!value || value.version !== 1 || !Array.isArray(value.buses)) {
    throw new Error("Некорректная конфигурация шин");
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
  return { version: 1, buses };
}

export function busFromEditorValues({ id, enabled, port, addresses }) {
  const numericId = typeof id === "number" ? id : Number(String(id).trim());
  return normalizeBus({
    id: numericId,
    enabled: enabled === true,
    port,
    addresses: parseAddressesInput(addresses),
  });
}

export function cloneConfiguration(value) {
  const normalized = normalizeConfiguration(value);
  return {
    version: 1,
    buses: normalized.buses.map((bus) => ({ ...bus, addresses: [...bus.addresses] })),
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
