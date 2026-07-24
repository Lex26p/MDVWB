const DASHBOARD_FITS = new Set(["contain", "width", "actual", "custom"]);
const IMAGE_EXTENSIONS = new Set(["png", "jpg", "jpeg", "webp"]);

function requireObject(value, description) {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new Error(`${description}: ожидался объект`);
  }
  return value;
}

function finiteNumber(value, minimum, maximum, description) {
  const number = Number(value);
  if (!Number.isFinite(number) || number < minimum || number > maximum) {
    throw new Error(`${description}: допустимо значение ${minimum}–${maximum}`);
  }
  return number;
}

function integer(value, minimum, maximum, description) {
  const number = Number(value);
  if (!Number.isInteger(number) || number < minimum || number > maximum) {
    throw new Error(`${description}: допустимо целое число ${minimum}–${maximum}`);
  }
  return number;
}

function text(value, minimum, maximum, description) {
  const result = String(value ?? "").trim();
  const length = new TextEncoder().encode(result).length;
  if (length < minimum || length > maximum) {
    throw new Error(`${description}: длина должна быть ${minimum}–${maximum} байт`);
  }
  return result;
}

function normalizeBackground(input) {
  const background = requireObject(input, "Фон панели");
  const fit = String(background.fit ?? "contain");
  if (!DASHBOARD_FITS.has(fit)) {
    throw new Error("Режим отображения фона не поддерживается");
  }

  return {
    file: String(background.file ?? ""),
    naturalWidth: integer(background.naturalWidth ?? 0, 0, 8192, "Ширина изображения"),
    naturalHeight: integer(background.naturalHeight ?? 0, 0, 8192, "Высота изображения"),
    defaultScale: finiteNumber(background.defaultScale ?? 1, 0.25, 4, "Масштаб изображения"),
    fit,
  };
}

function inferredFanNumber(fan, index) {
  if (fan.number !== undefined && fan.number !== null && fan.number !== "") {
    return fan.number;
  }
  const label = String(fan.label ?? "");
  const match = label.match(/№\s*(\d{1,3})/u) || label.match(/фанкойл\s*(\d{1,3})/iu);
  return match ? Number(match[1]) : index + 1;
}

function normalizeFan(input, index) {
  const fan = requireObject(input, `Маркер ${index + 1}`);
  const id = text(fan.id, 1, 64, `ID маркера ${index + 1}`);
  if (!/^[A-Za-z0-9_-]+$/.test(id)) {
    throw new Error(`ID маркера ${index + 1}: допустимы только буквы, цифры, _ и -`);
  }
  return {
    id,
    number: integer(inferredFanNumber(fan, index), 1, 200, `Номер фанкойла ${index + 1}`),
    bus: integer(fan.bus, 1, 999, `Шина маркера ${index + 1}`),
    address: integer(fan.address, 0, 63, `Адрес маркера ${index + 1}`),
    label: text(fan.label, 1, 120, `Подпись маркера ${index + 1}`),
    x: finiteNumber(fan.x, 0, 1, `Координата X маркера ${index + 1}`),
    y: finiteNumber(fan.y, 0, 1, `Координата Y маркера ${index + 1}`),
    markerScale: finiteNumber(fan.markerScale, 0.5, 3, `Размер маркера ${index + 1}`),
    rotation: 0,
    visible: fan.visible === true,
  };
}

export function emptyDashboardConfiguration() {
  return {
    version: 1,
    revision: 0,
    title: "Панель фанкойлов",
    background: {
      file: "",
      naturalWidth: 0,
      naturalHeight: 0,
      defaultScale: 1,
      fit: "contain",
    },
    fans: [],
  };
}

export function emptyDashboardPanel(id = "main", title = "Панель фанкойлов") {
  const dashboard = emptyDashboardConfiguration();
  return {
    id: panelIdentifier(id),
    title: text(title, 1, 160, "Название панели"),
    background: { ...dashboard.background },
    fans: [],
  };
}

export function emptyDashboardCollection() {
  return {
    version: 2,
    revision: 0,
    defaultPanel: "main",
    panels: [emptyDashboardPanel()],
  };
}

export function panelIdentifier(value, description = "ID панели") {
  const id = text(value, 1, 48, description);
  if (!/^[A-Za-z0-9_-]+$/.test(id)) {
    throw new Error(`${description}: допустимы только латинские буквы, цифры, _ и -`);
  }
  return id;
}

function normalizeDashboardPanel(input, index) {
  const panel = requireObject(input, `Панель ${index + 1}`);
  const normalized = normalizeDashboardConfiguration({
    version: 1,
    revision: 0,
    title: panel.title,
    background: panel.background,
    fans: panel.fans,
  });
  return {
    id: panelIdentifier(panel.id, `ID панели ${index + 1}`),
    title: normalized.title,
    background: normalized.background,
    fans: normalized.fans,
  };
}

export function normalizeDashboardCollection(input) {
  const root = requireObject(input, "Конфигурация веб-панелей");
  if (Number(root.version) === 1) {
    const legacy = normalizeDashboardConfiguration(root);
    return {
      version: 2,
      revision: legacy.revision,
      defaultPanel: "main",
      panels: [{
        id: "main",
        title: legacy.title,
        background: { ...legacy.background },
        fans: legacy.fans.map((fan) => ({ ...fan })),
      }],
    };
  }
  if (Number(root.version) !== 2) {
    throw new Error("Версия конфигурации веб-панелей должна быть равна 2");
  }
  const panels = Array.isArray(root.panels) ? root.panels.map(normalizeDashboardPanel) : [];
  if (panels.length < 1 || panels.length > 64) {
    throw new Error("Допускается от 1 до 64 пользовательских панелей");
  }
  const ids = new Set();
  panels.forEach((panel) => {
    if (ids.has(panel.id)) {
      throw new Error(`ID панели «${panel.id}» указан повторно`);
    }
    ids.add(panel.id);
  });
  const defaultPanel = panelIdentifier(root.defaultPanel, "Панель по умолчанию");
  if (!ids.has(defaultPanel)) {
    throw new Error("Панель по умолчанию не существует");
  }
  return {
    version: 2,
    revision: integer(root.revision ?? 0, 0, 2147483647, "Revision панелей"),
    defaultPanel,
    panels,
  };
}

export function cloneDashboardCollection(input) {
  const collection = normalizeDashboardCollection(input);
  return {
    ...collection,
    panels: collection.panels.map((panel) => ({
      ...panel,
      background: { ...panel.background },
      fans: panel.fans.map((fan) => ({ ...fan })),
    })),
  };
}

export function dashboardCollectionToJson(input) {
  return JSON.stringify(normalizeDashboardCollection(input));
}
export function dashboardCollectionsEqual(left, right) {
  return JSON.stringify(normalizeDashboardCollection(left)) ===
    JSON.stringify(normalizeDashboardCollection(right));
}


export function findDashboardPanel(collection, id) {
  const normalized = normalizeDashboardCollection(collection);
  const requested = String(id || normalized.defaultPanel);
  const index = normalized.panels.findIndex((panel) => panel.id === requested);
  if (index < 0) {
    return null;
  }
  // Version-2 callers (the editor and operator page) already hold a normalized
  // collection. Return their actual panel object so deliberate edits mutate the
  // current draft instead of a temporary normalized copy.
  if (Number(collection?.version) === 2 && Array.isArray(collection.panels)) {
    return collection.panels[index] || null;
  }
  return normalized.panels[index];
}

export function dashboardPanelToConfiguration(panel, revision = 0) {
  const normalized = normalizeDashboardPanel(panel, 0);
  return {
    version: 1,
    revision: integer(revision, 0, 2147483647, "Revision панелей"),
    title: normalized.title,
    background: { ...normalized.background },
    fans: normalized.fans.map((fan) => ({ ...fan })),
  };
}

export function dashboardConfigurationToPanel(configuration, id) {
  const dashboard = normalizeDashboardConfiguration(configuration);
  return {
    id: panelIdentifier(id),
    title: dashboard.title,
    background: { ...dashboard.background },
    fans: dashboard.fans.map((fan) => ({ ...fan })),
  };
}

export function panelUserUrl(id, base = "/fancoils/") {
  return `${base}?panel=${encodeURIComponent(panelIdentifier(id))}`;
}

export function nextPanelIdentifier(collection) {
  const normalized = normalizeDashboardCollection(collection);
  const ids = new Set(normalized.panels.map((panel) => panel.id));
  for (let number = 1; number <= 999; number += 1) {
    const candidate = `panel-${number}`;
    if (!ids.has(candidate)) {
      return candidate;
    }
  }
  throw new Error("Не удалось подобрать свободный ID панели");
}

export function normalizeDashboardConfiguration(input) {
  const dashboard = requireObject(input, "Конфигурация панели");
  if (Number(dashboard.version) !== 1) {
    throw new Error("Версия конфигурации панели должна быть равна 1");
  }
  const fans = Array.isArray(dashboard.fans) ? dashboard.fans : [];
  if (fans.length > 4096) {
    throw new Error("На одной панели допускается не более 4096 маркеров");
  }

  const normalizedFans = fans.map(normalizeFan);
  const ids = new Set();
  const numbers = new Set();
  const devices = new Set();
  normalizedFans.forEach((fan) => {
    if (ids.has(fan.id)) {
      throw new Error(`ID маркера «${fan.id}» указан повторно`);
    }
    ids.add(fan.id);
    if (numbers.has(fan.number)) {
      throw new Error(`Номер фанкойла ${fan.number} указан повторно`);
    }
    numbers.add(fan.number);
    const key = deviceKey(fan.bus, fan.address);
    if (devices.has(key)) {
      throw new Error(`Устройство Fan-${fan.bus}_${fan.address} размещено повторно`);
    }
    devices.add(key);
  });

  return {
    version: 1,
    revision: integer(dashboard.revision ?? 0, 0, 2147483647, "Revision панели"),
    title: text(dashboard.title, 1, 160, "Название панели"),
    background: normalizeBackground(dashboard.background ?? {}),
    fans: normalizedFans,
  };
}

export function cloneDashboardConfiguration(input) {
  const dashboard = normalizeDashboardConfiguration(input);
  return {
    ...dashboard,
    background: { ...dashboard.background },
    fans: dashboard.fans.map((fan) => ({ ...fan })),
  };
}

export function dashboardConfigurationsEqual(left, right) {
  return JSON.stringify(normalizeDashboardConfiguration(left)) ===
    JSON.stringify(normalizeDashboardConfiguration(right));
}

export function dashboardConfigurationToJson(input) {
  return JSON.stringify(normalizeDashboardConfiguration(input));
}

export function parseDashboardPayload(payload, description = "данные панели") {
  try {
    return JSON.parse(String(payload));
  } catch (error) {
    throw new Error(`Не удалось разобрать ${description}: ${error.message}`);
  }
}

export function dashboardAssetUrl(fileName) {
  const file = String(fileName ?? "");
  if (!file) {
    return "";
  }
  if (!/^[A-Za-z0-9_.-]+$/.test(file)) {
    throw new Error("Имя файла подложки содержит недопустимые символы");
  }
  return `/fancoils/assets/${encodeURIComponent(file)}`;
}

export function fitLabel(value) {
  return {
    contain: "Вписать в окно",
    width: "По ширине",
    actual: "Фактический размер",
    custom: "Пользовательский масштаб",
  }[value] || value;
}

export function formatBytes(bytes) {
  const value = Number(bytes) || 0;
  if (value < 1024) {
    return `${value} Б`;
  }
  if (value < 1024 * 1024) {
    return `${(value / 1024).toFixed(value < 10 * 1024 ? 1 : 0)} КиБ`;
  }
  return `${(value / (1024 * 1024)).toFixed(2)} МиБ`;
}

export function createUploadId() {
  const random = Math.random().toString(36).slice(2, 10);
  return `web-${Date.now().toString(36)}-${random}`;
}

export function validateBackgroundFile(file) {
  if (!(file instanceof File)) {
    throw new Error("Выберите изображение");
  }
  if (file.size < 1 || file.size > 10 * 1024 * 1024) {
    throw new Error("Размер изображения должен быть от 1 байта до 10 МиБ");
  }
  const dot = file.name.lastIndexOf(".");
  const extension = dot >= 0 ? file.name.slice(dot + 1).toLowerCase() : "";
  if (!IMAGE_EXTENSIONS.has(extension)) {
    throw new Error("Поддерживаются только PNG, JPEG и WebP");
  }
  return file;
}

export function safeUploadFileName(fileName) {
  const name = String(fileName ?? "");
  const dot = name.lastIndexOf(".");
  const extension = dot >= 0 ? name.slice(dot + 1).toLowerCase() : "png";
  const rawBase = dot > 0 ? name.slice(0, dot) : "background";
  let base = rawBase
    .normalize("NFKD")
    .replace(/[^A-Za-z0-9_-]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 96);
  if (!base) {
    base = "background";
  }
  return `${base}.${extension}`;
}

export function readImageDimensions(file) {
  return new Promise((resolve, reject) => {
    const url = URL.createObjectURL(file);
    const image = new Image();
    image.onload = () => {
      const width = image.naturalWidth;
      const height = image.naturalHeight;
      URL.revokeObjectURL(url);
      if (width < 1 || height < 1 || width > 8192 || height > 8192) {
        reject(new Error("Размер изображения должен быть от 1×1 до 8192×8192"));
        return;
      }
      resolve({ width, height });
    };
    image.onerror = () => {
      URL.revokeObjectURL(url);
      reject(new Error("Браузер не смог прочитать выбранное изображение"));
    };
    image.src = url;
  });
}

function rotateRight(value, shift) {
  return (value >>> shift) | (value << (32 - shift));
}

export function sha256HexBytes(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input);
  const bitLength = bytes.length * 8;
  const paddedLength = Math.ceil((bytes.length + 9) / 64) * 64;
  const padded = new Uint8Array(paddedLength);
  padded.set(bytes);
  padded[bytes.length] = 0x80;

  const view = new DataView(padded.buffer);
  const high = Math.floor(bitLength / 0x100000000);
  const low = bitLength >>> 0;
  view.setUint32(paddedLength - 8, high, false);
  view.setUint32(paddedLength - 4, low, false);

  const constants = new Uint32Array([
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
  ]);

  let h0 = 0x6a09e667;
  let h1 = 0xbb67ae85;
  let h2 = 0x3c6ef372;
  let h3 = 0xa54ff53a;
  let h4 = 0x510e527f;
  let h5 = 0x9b05688c;
  let h6 = 0x1f83d9ab;
  let h7 = 0x5be0cd19;
  const words = new Uint32Array(64);

  for (let offset = 0; offset < padded.length; offset += 64) {
    for (let index = 0; index < 16; index += 1) {
      words[index] = view.getUint32(offset + index * 4, false);
    }
    for (let index = 16; index < 64; index += 1) {
      const w15 = words[index - 15];
      const w2 = words[index - 2];
      const s0 = rotateRight(w15, 7) ^ rotateRight(w15, 18) ^ (w15 >>> 3);
      const s1 = rotateRight(w2, 17) ^ rotateRight(w2, 19) ^ (w2 >>> 10);
      words[index] = (words[index - 16] + s0 + words[index - 7] + s1) >>> 0;
    }

    let a = h0;
    let b = h1;
    let c = h2;
    let d = h3;
    let e = h4;
    let f = h5;
    let g = h6;
    let h = h7;

    for (let index = 0; index < 64; index += 1) {
      const s1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
      const choose = (e & f) ^ (~e & g);
      const temp1 = (h + s1 + choose + constants[index] + words[index]) >>> 0;
      const s0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
      const majority = (a & b) ^ (a & c) ^ (b & c);
      const temp2 = (s0 + majority) >>> 0;
      h = g;
      g = f;
      f = e;
      e = (d + temp1) >>> 0;
      d = c;
      c = b;
      b = a;
      a = (temp1 + temp2) >>> 0;
    }

    h0 = (h0 + a) >>> 0;
    h1 = (h1 + b) >>> 0;
    h2 = (h2 + c) >>> 0;
    h3 = (h3 + d) >>> 0;
    h4 = (h4 + e) >>> 0;
    h5 = (h5 + f) >>> 0;
    h6 = (h6 + g) >>> 0;
    h7 = (h7 + h) >>> 0;
  }

  return [h0, h1, h2, h3, h4, h5, h6, h7]
    .map((value) => value.toString(16).padStart(8, "0"))
    .join("");
}

export async function sha256HexFile(file) {
  const bytes = new Uint8Array(await file.arrayBuffer());
  if (globalThis.crypto?.subtle) {
    try {
      const digest = await globalThis.crypto.subtle.digest("SHA-256", bytes);
      return [...new Uint8Array(digest)]
        .map((byte) => byte.toString(16).padStart(2, "0"))
        .join("");
    } catch (_) {
      // HTTP installations may not expose Web Crypto. Use the local implementation below.
    }
  }
  return sha256HexBytes(bytes);
}


export function deviceKey(bus, address) {
  return `${integer(Number(bus), 1, 999, "Номер шины")}:${integer(Number(address), 0, 63, "Адрес фанкойла")}`;
}

export function availableDashboardDevices(configuration) {
  if (!configuration || Number(configuration.version) !== 1 || !Array.isArray(configuration.buses)) {
    return [];
  }
  const devices = [];
  configuration.buses.forEach((bus) => {
    const busId = integer(Number(bus.id), 1, 999, "Номер шины");
    const addresses = Array.isArray(bus.addresses) ? bus.addresses : [];
    addresses.forEach((address) => {
      const normalizedAddress = integer(Number(address), 0, 63, "Адрес фанкойла");
      devices.push({
        key: deviceKey(busId, normalizedAddress),
        bus: busId,
        address: normalizedAddress,
        enabled: bus.enabled === true,
        name: `Fan-${busId}_${normalizedAddress}`,
      });
    });
  });
  devices.sort((left, right) => left.bus - right.bus || left.address - right.address);
  return devices;
}

export function inspectDashboardPlacement(fan, configuration) {
  const buses = configuration && Array.isArray(configuration.buses) ? configuration.buses : [];
  const bus = buses.find((item) => Number(item.id) === Number(fan.bus));
  if (!bus) {
    return { kind: "missingBus", message: `Шина ${fan.bus} отсутствует в конфигурации` };
  }
  if (!Array.isArray(bus.addresses) || !bus.addresses.some((address) => Number(address) === Number(fan.address))) {
    return {
      kind: "missingAddress",
      message: `Адрес ${fan.address} отсутствует на шине ${fan.bus}`,
    };
  }
  return null;
}

function uniquePlacementId(base, existingFans) {
  const safeBase = String(base)
    .replace(/[^A-Za-z0-9_-]+/g, "-")
    .replace(/^-+|-+$/g, "")
    .slice(0, 56) || "fan";
  const used = new Set((existingFans || []).map((fan) => String(fan.id)));
  if (!used.has(safeBase)) {
    return safeBase;
  }
  for (let suffix = 2; suffix < 100000; suffix += 1) {
    const candidate = `${safeBase.slice(0, 63 - String(suffix).length)}-${suffix}`;
    if (!used.has(candidate)) {
      return candidate;
    }
  }
  throw new Error("Не удалось подобрать уникальный ID маркера");
}

export function nextAvailableFanNumber(existingFans = []) {
  const used = new Set((existingFans || []).map((fan) => Number(fan.number)).filter(Number.isInteger));
  for (let number = 1; number <= 200; number += 1) {
    if (!used.has(number)) {
      return number;
    }
  }
  throw new Error("Все пользовательские номера 1–200 уже заняты");
}

export function createFanPlacement({ bus, address, number, label, x = 0.5, y = 0.5 }, existingFans = []) {
  const normalizedBus = integer(Number(bus), 1, 999, "Номер шины");
  const normalizedAddress = integer(Number(address), 0, 63, "Адрес фанкойла");
  if ((existingFans || []).some((fan) => Number(fan.bus) === normalizedBus && Number(fan.address) === normalizedAddress)) {
    throw new Error(`Устройство Fan-${normalizedBus}_${normalizedAddress} уже размещено`);
  }
  const assignedNumber = number === undefined || number === null
    ? nextAvailableFanNumber(existingFans)
    : integer(Number(number), 1, 200, "Номер фанкойла");
  if ((existingFans || []).some((fan) => Number(fan.number) === assignedNumber)) {
    throw new Error(`Номер фанкойла ${assignedNumber} уже используется`);
  }
  return normalizeFan({
    id: uniquePlacementId(`fan-${normalizedBus}-${normalizedAddress}`, existingFans),
    number: assignedNumber,
    bus: normalizedBus,
    address: normalizedAddress,
    label: label || `Фанкойл ${assignedNumber}`,
    x,
    y,
    markerScale: 1,
    rotation: 0,
    visible: true,
  }, existingFans.length);
}
