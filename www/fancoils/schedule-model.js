const SAFE_ID = /^[A-Za-z0-9_-]{1,64}$/;
const DAY_LABELS = Object.freeze({
  1: "Пн",
  2: "Вт",
  3: "Ср",
  4: "Чт",
  5: "Пт",
  6: "Сб",
  7: "Вс",
});

function asInteger(value, minimum, maximum, label) {
  const number = Number(value);
  if (!Number.isInteger(number) || number < minimum || number > maximum) {
    throw new Error(`${label}: ожидается целое число ${minimum}..${maximum}`);
  }
  return number;
}

function normalizeText(value, label, maximum = 128) {
  const text = String(value ?? "").trim();
  if (!text || new TextEncoder().encode(text).length > maximum) {
    throw new Error(`${label}: требуется непустой текст до ${maximum} байт`);
  }
  return text;
}

function normalizeIdentifier(value, label = "ID расписания") {
  const text = String(value ?? "").trim();
  if (!SAFE_ID.test(text)) {
    throw new Error(`${label}: разрешены A-Z, a-z, 0-9, _ и -`);
  }
  return text;
}

function normalizeTime(value) {
  const text = String(value ?? "").trim();
  const match = /^(\d{2}):(\d{2})$/.exec(text);
  if (!match || Number(match[1]) > 23 || Number(match[2]) > 59) {
    throw new Error("Время должно быть в формате HH:MM");
  }
  return text;
}

function normalizeDate(value) {
  const text = String(value ?? "").trim();
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(text);
  if (!match) {
    throw new Error("Дата должна быть в формате YYYY-MM-DD");
  }
  const year = Number(match[1]);
  const month = Number(match[2]);
  const day = Number(match[3]);
  const date = new Date(year, month - 1, day);
  if (date.getFullYear() !== year || date.getMonth() !== month - 1 || date.getDate() !== day) {
    throw new Error("Указана несуществующая дата");
  }
  return text;
}

export function scheduleTargetKey(bus, address) {
  return `${Number(bus)}:${Number(address)}`;
}

function normalizeTarget(target) {
  return {
    bus: asInteger(target?.bus, 1, 999, "Номер шины"),
    address: asInteger(target?.address, 0, 63, "Адрес фанкойла"),
  };
}

function normalizeActions(actions) {
  const source = actions && typeof actions === "object" ? actions : {};
  const result = {};
  if (source.power !== undefined && source.power !== null) {
    if (source.power !== true && source.power !== false) {
      throw new Error("Питание должно быть true или false");
    }
    result.power = source.power;
  }
  if (source.mode !== undefined && source.mode !== null) {
    result.mode = asInteger(source.mode, 0, 4, "Режим");
  }
  if (source.speed !== undefined && source.speed !== null) {
    result.speed = asInteger(source.speed, 1, 4, "Скорость");
  }
  if (source.setTemp !== undefined && source.setTemp !== null) {
    result.setTemp = asInteger(source.setTemp, 16, 32, "Уставка");
  }
  if (!Object.keys(result).length) {
    throw new Error("Выберите хотя бы один параметр расписания");
  }
  return result;
}

export function normalizeSchedule(entry) {
  const kind = String(entry?.kind || "weekly");
  if (kind !== "weekly" && kind !== "once") {
    throw new Error("Тип расписания должен быть weekly или once");
  }

  const targets = (Array.isArray(entry?.targets) ? entry.targets : []).map(normalizeTarget);
  if (!targets.length) {
    throw new Error("Выберите хотя бы один фанкойл");
  }
  const seenTargets = new Set();
  targets.forEach((target) => {
    const key = scheduleTargetKey(target.bus, target.address);
    if (seenTargets.has(key)) {
      throw new Error(`Фанкойл ${key} выбран повторно`);
    }
    seenTargets.add(key);
  });
  targets.sort((left, right) => left.bus - right.bus || left.address - right.address);

  const days = [...new Set((Array.isArray(entry?.days) ? entry.days : []).map((day) => asInteger(day, 1, 7, "День недели")))].sort();
  const date = kind === "once" ? normalizeDate(entry?.date) : "";
  if (kind === "weekly" && !days.length) {
    throw new Error("Для еженедельного расписания выберите дни недели");
  }

  return {
    id: normalizeIdentifier(entry?.id),
    name: normalizeText(entry?.name, "Название расписания"),
    enabled: entry?.enabled !== false,
    panelId: normalizeIdentifier(entry?.panelId || "main", "ID панели"),
    kind,
    days: kind === "weekly" ? days : [],
    date,
    time: normalizeTime(entry?.time || "00:00"),
    targets,
    actions: normalizeActions(entry?.actions),
  };
}

export function normalizeSchedulesConfiguration(input) {
  const source = typeof input === "string" ? JSON.parse(input) : input;
  if (!source || typeof source !== "object" || Number(source.version) !== 1) {
    throw new Error("Конфигурация расписаний должна иметь version = 1");
  }
  const schedules = (Array.isArray(source.schedules) ? source.schedules : []).map(normalizeSchedule);
  if (schedules.length > 256) {
    throw new Error("Допускается не более 256 расписаний");
  }
  const ids = new Set();
  schedules.forEach((schedule) => {
    if (ids.has(schedule.id)) {
      throw new Error(`ID расписания ${schedule.id} указан повторно`);
    }
    ids.add(schedule.id);
  });
  return {
    version: 1,
    revision: asInteger(source.revision ?? 0, 0, 2147483647, "Ревизия"),
    schedules,
  };
}

export function emptySchedulesConfiguration() {
  return { version: 1, revision: 0, schedules: [] };
}

export function cloneSchedule(schedule) {
  return JSON.parse(JSON.stringify(schedule));
}

export function cloneSchedulesConfiguration(config) {
  return normalizeSchedulesConfiguration(JSON.parse(JSON.stringify(config)));
}

export function schedulesConfigurationToJson(config) {
  return JSON.stringify(normalizeSchedulesConfiguration(config));
}

export function schedulesForPanel(config, panelId) {
  const id = String(panelId || "");
  return normalizeSchedulesConfiguration(config).schedules.filter((schedule) => schedule.panelId === id);
}

export function nextScheduleIdentifier(config, prefix = "schedule") {
  const ids = new Set((config?.schedules || []).map((schedule) => String(schedule.id)));
  for (let number = 1; number <= 9999; number += 1) {
    const id = `${prefix}-${number}`;
    if (!ids.has(id)) {
      return id;
    }
  }
  throw new Error("Не удалось подобрать свободный ID расписания");
}

export function createSchedule(panelId, config, now = new Date()) {
  const date = new Date(now);
  date.setMinutes(date.getMinutes() + 5, 0, 0);
  return {
    id: nextScheduleIdentifier(config),
    name: "Новое расписание",
    enabled: true,
    panelId: normalizeIdentifier(panelId || "main", "ID панели"),
    kind: "weekly",
    days: [1, 2, 3, 4, 5],
    date: "",
    time: `${String(date.getHours()).padStart(2, "0")}:${String(date.getMinutes()).padStart(2, "0")}`,
    targets: [],
    actions: { power: true },
  };
}

export function targetSet(schedule) {
  return new Set((schedule?.targets || []).map((target) => scheduleTargetKey(target.bus, target.address)));
}

export function setScheduleTarget(schedule, bus, address, enabled) {
  const key = scheduleTargetKey(bus, address);
  const targets = (schedule.targets || []).filter((target) => scheduleTargetKey(target.bus, target.address) !== key);
  if (enabled) {
    targets.push(normalizeTarget({ bus, address }));
  }
  schedule.targets = targets.sort((left, right) => left.bus - right.bus || left.address - right.address);
  return schedule;
}

export function scheduleDaysLabel(schedule) {
  if (schedule.kind === "once") {
    return schedule.date;
  }
  const days = schedule.days || [];
  if (days.join(",") === "1,2,3,4,5") {
    return "По будням";
  }
  if (days.join(",") === "6,7") {
    return "По выходным";
  }
  if (days.length === 7) {
    return "Каждый день";
  }
  return days.map((day) => DAY_LABELS[day]).join(", ");
}

export function scheduleTimingLabel(schedule) {
  return `${scheduleDaysLabel(schedule)}, ${schedule.time}`;
}

export function nextScheduleRun(schedule, nowInput = new Date()) {
  if (!schedule?.enabled) {
    return null;
  }
  const now = new Date(nowInput);
  const [hour, minute] = String(schedule.time || "00:00").split(":").map(Number);
  if (schedule.kind === "once") {
    const [year, month, day] = String(schedule.date || "").split("-").map(Number);
    const candidate = new Date(year, month - 1, day, hour, minute, 0, 0);
    return Number.isFinite(candidate.getTime()) && candidate >= now ? candidate : null;
  }

  const days = new Set(schedule.days || []);
  for (let offset = 0; offset <= 7; offset += 1) {
    const candidate = new Date(now);
    candidate.setDate(now.getDate() + offset);
    candidate.setHours(hour, minute, 0, 0);
    const weekday = candidate.getDay() === 0 ? 7 : candidate.getDay();
    if (days.has(weekday) && candidate >= now) {
      return candidate;
    }
  }
  return null;
}

export function nextRunLabel(schedule, now = new Date()) {
  if (!schedule.enabled) {
    return "Автозапуск выключен";
  }
  const next = nextScheduleRun(schedule, now);
  if (!next) {
    return schedule.kind === "once" ? "Время запуска прошло" : "Следующий запуск не определён";
  }
  return `Следующий запуск: ${next.toLocaleString("ru-RU", {
    weekday: "short",
    day: "2-digit",
    month: "2-digit",
    hour: "2-digit",
    minute: "2-digit",
  })}`;
}

export function parseScheduleResultTopic(topic) {
  const match = /^\/mdvwb\/schedules\/([A-Za-z0-9_-]{1,64})\/result$/.exec(String(topic));
  return match ? match[1] : null;
}

export function runStateLabel(state) {
  const labels = {
    queued: "Поставлено в очередь",
    executing: "Выполняется",
    completed: "Выполнено",
    timeout: "Нет подтверждения",
    failed: "Ошибка",
    rejected: "Отклонено",
  };
  return labels[String(state)] || String(state || "Нет запусков");
}
