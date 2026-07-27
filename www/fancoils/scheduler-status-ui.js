import { TinyMqttClient } from "../mdvwb/mqtt-client.js";

const SCHEDULER_STATUS_TOPIC = "/mdvwb/scheduler/status";
const SCHEDULE_RESULT_FILTER = "/mdvwb/schedules/+/result";
const RESULT_TOPIC_PATTERN = /^\/mdvwb\/schedules\/([A-Za-z0-9_-]{1,64})\/result$/;
const TERMINAL_STATES = new Set(["completed", "timeout", "failed", "rejected", "missed"]);
const WEEKDAYS = Object.freeze({
  1: "Пн",
  2: "Вт",
  3: "Ср",
  4: "Чт",
  5: "Пт",
  6: "Сб",
  7: "Вс",
});

let installed = false;

function parseObject(payload, description) {
  let result;
  try {
    result = JSON.parse(String(payload));
  } catch (_error) {
    throw new Error(`Некорректный JSON: ${description}`);
  }
  if (!result || typeof result !== "object" || Array.isArray(result)) {
    throw new Error(`Некорректные данные: ${description}`);
  }
  return result;
}

function formatControllerDate(value) {
  const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(value || ""));
  return match ? `${match[3]}.${match[2]}.${match[1]}` : "";
}

export function controllerClockLabel(status) {
  const date = formatControllerDate(status?.controllerDate);
  const time = /^\d{2}:\d{2}$/.test(String(status?.controllerTime || ""))
    ? String(status.controllerTime)
    : "";
  if (!date || !time) {
    return "Время WB: —";
  }
  const weekday = WEEKDAYS[Number(status?.controllerWeekday)] || "";
  return `Время WB: ${date} ${time}${weekday ? ` · ${weekday}` : ""}`;
}

function resultTimeSuffix(result) {
  const minute = String(result?.controllerMinute || "");
  const match = /^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2})$/.exec(minute);
  return match ? ` · WB ${match[3]}.${match[2]}.${match[1]} ${match[4]}:${match[5]}` : "";
}

function resultCounters(result) {
  const commands = Number(result?.commands);
  const confirmed = Number(result?.confirmed);
  if (!Number.isInteger(commands) || !Number.isInteger(confirmed)) {
    return "";
  }
  if (commands === 0) {
    return " · состояние уже соответствовало заданию";
  }
  return ` · подтверждено ${confirmed}/${commands}`;
}

export function schedulerRunMessage(result) {
  const state = String(result?.state || "");
  const message = String(result?.message || "").trim();
  const suffix = resultTimeSuffix(result);
  if (state === "queued") {
    return {
      kind: "info",
      text: `Scheduler подтвердил постановку расписания в очередь${suffix}.`,
      terminal: false,
    };
  }
  if (state === "executing") {
    return {
      kind: "info",
      text: `Расписание выполняется${resultCounters(result)}${suffix}.`,
      terminal: false,
    };
  }
  if (state === "completed") {
    return {
      kind: "success",
      text: `Расписание выполнено${resultCounters(result)}${suffix}.`,
      terminal: true,
    };
  }
  const labels = {
    timeout: "Нет фактического подтверждения",
    failed: "Выполнение завершилось ошибкой",
    rejected: "Scheduler отклонил запуск",
    missed: "Запуск расписания пропущен",
  };
  return {
    kind: state === "failed" || state === "rejected" ? "error" : "warning",
    text: `${labels[state] || "Получен результат scheduler"}${message ? `: ${message}` : ""}${suffix}.`,
    terminal: TERMINAL_STATES.has(state),
  };
}

function appendStyles() {
  if (document.getElementById("schedulerStatusUiStyles")) {
    return;
  }
  const style = document.createElement("style");
  style.id = "schedulerStatusUiStyles";
  style.textContent = `
    .scheduler-controller-clock {
      display: inline-flex;
      align-items: center;
      min-height: 28px;
      padding: 0 9px;
      border: 1px solid color-mix(in srgb, currentColor 18%, transparent);
      border-radius: 999px;
      font-size: 12px;
      font-weight: 650;
      white-space: nowrap;
      opacity: .82;
    }
    .scheduler-controller-clock.scheduler-clock-stale { opacity: .58; }
    #scheduleRunButton[data-scheduler-waiting="true"] { cursor: progress; }
    #scheduleRunButton[data-scheduler-waiting="true"]::after {
      content: "";
      display: inline-block;
      width: 10px;
      height: 10px;
      margin-left: 7px;
      border: 2px solid currentColor;
      border-right-color: transparent;
      border-radius: 50%;
      animation: scheduler-status-spin .75s linear infinite;
      vertical-align: -1px;
    }
    @keyframes scheduler-status-spin { to { transform: rotate(360deg); } }
  `;
  document.head.appendChild(style);
}

function createClockElement(statusBadge) {
  let clock = document.getElementById("schedulerControllerClock");
  if (clock) {
    return clock;
  }
  clock = document.createElement("span");
  clock.id = "schedulerControllerClock";
  clock.className = "scheduler-controller-clock scheduler-clock-stale";
  clock.textContent = "Время WB: —";
  clock.title = "Локальные дата и время контроллера Wiren Board";
  statusBadge.insertAdjacentElement("afterend", clock);
  return clock;
}

function applyFeedback(feedback, presentation) {
  feedback.textContent = presentation.text;
  feedback.className = `schedule-feedback schedule-feedback-${presentation.kind}`;
}

export function installSchedulerStatusUi() {
  if (installed) {
    return;
  }
  installed = true;

  const statusBadge = document.getElementById("schedulerStatusBadge");
  const runButton = document.getElementById("scheduleRunButton");
  const feedback = document.getElementById("scheduleEditorFeedback");
  if (!statusBadge || !runButton || !feedback) {
    console.warn("Scheduler status UI: required elements are missing");
    return;
  }

  appendStyles();
  const clockElement = createClockElement(statusBadge);
  const demo = new URLSearchParams(window.location.search).get("demo") === "1";
  let schedulerStatus = null;
  let bridgeConnected = false;
  let feedbackOverride = null;
  let overrideTimer = 0;
  let safetyTimer = 0;
  const manualRun = {
    active: false,
    scheduleId: "",
    schedulerConfirmed: false,
  };

  const renderClock = () => {
    if (demo) {
      const now = new Date();
      clockElement.textContent = `Время WB (демо): ${now.toLocaleString("ru-RU", {
        day: "2-digit",
        month: "2-digit",
        year: "numeric",
        hour: "2-digit",
        minute: "2-digit",
      })}`;
      clockElement.classList.remove("scheduler-clock-stale");
      return;
    }
    clockElement.textContent = controllerClockLabel(schedulerStatus);
    clockElement.classList.toggle("scheduler-clock-stale", !bridgeConnected || !schedulerStatus);
    if (schedulerStatus?.message) {
      clockElement.title = String(schedulerStatus.message);
    }
  };

  const enforceManualState = () => {
    if (manualRun.active) {
      runButton.disabled = true;
      runButton.dataset.schedulerWaiting = "true";
    } else {
      delete runButton.dataset.schedulerWaiting;
    }
    if (feedbackOverride && feedback.textContent !== feedbackOverride.text) {
      applyFeedback(feedback, feedbackOverride);
    }
  };

  const setOverride = (presentation, keepMilliseconds = 0) => {
    window.clearTimeout(overrideTimer);
    feedbackOverride = presentation;
    applyFeedback(feedback, presentation);
    if (keepMilliseconds > 0) {
      overrideTimer = window.setTimeout(() => {
        feedbackOverride = null;
      }, keepMilliseconds);
    }
  };

  const finishManualRun = (presentation) => {
    manualRun.active = false;
    manualRun.scheduleId = "";
    manualRun.schedulerConfirmed = false;
    window.clearTimeout(safetyTimer);
    delete runButton.dataset.schedulerWaiting;
    setOverride(presentation, 7000);
  };

  const beginManualRun = () => {
    if (runButton.disabled) {
      return;
    }
    manualRun.active = true;
    manualRun.scheduleId = "";
    manualRun.schedulerConfirmed = false;
    setOverride({
      kind: "info",
      text: "Запрос ручного запуска отправлен manager. Ожидается подтверждение scheduler…",
    });
    enforceManualState();
    window.clearTimeout(safetyTimer);
    safetyTimer = window.setTimeout(() => {
      if (!manualRun.active) {
        return;
      }
      finishManualRun({
        kind: "warning",
        text: "Подтверждение scheduler не получено. Проверьте MQTT и состояние службы.",
      });
    }, 90000);
  };

  runButton.addEventListener("click", beginManualRun, { capture: true });

  const observer = new MutationObserver(enforceManualState);
  observer.observe(feedback, { childList: true, characterData: true, subtree: true });
  observer.observe(runButton, { attributes: true, attributeFilter: ["disabled", "data-scheduler-waiting"] });

  if (demo) {
    renderClock();
    window.setInterval(renderClock, 30000);
    return;
  }

  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const client = new TinyMqttClient({
    url: `${protocol}//${window.location.host}/mqtt`,
    clientId: `mdvwb-scheduler-status-${Math.random().toString(16).slice(2, 10)}`,
    keepAliveSeconds: 30,
    reconnectDelayMs: 2000,
  });
  client.subscribe(SCHEDULER_STATUS_TOPIC);
  client.subscribe(SCHEDULE_RESULT_FILTER);

  client.onConnect = () => {
    bridgeConnected = true;
    renderClock();
  };
  client.onDisconnect = () => {
    bridgeConnected = false;
    renderClock();
  };
  client.onError = (error) => {
    clockElement.title = `Не удалось получить время контроллера: ${error.message}`;
  };
  client.onMessage = (topic, payload) => {
    try {
      if (topic === SCHEDULER_STATUS_TOPIC) {
        schedulerStatus = parseObject(payload, "состояние scheduler");
        renderClock();
        return;
      }

      const match = RESULT_TOPIC_PATTERN.exec(String(topic));
      if (!match || !manualRun.active) {
        return;
      }
      const result = parseObject(payload, "результат запуска расписания");
      const scheduleId = String(result.scheduleId || match[1]);
      if (manualRun.scheduleId && manualRun.scheduleId !== scheduleId) {
        return;
      }
      manualRun.scheduleId = scheduleId;

      if (result.origin !== "scheduler") {
        if (result.success === false) {
          finishManualRun({
            kind: "error",
            text: `Manager отклонил ручной запуск: ${result.message || "причина не указана"}.`,
          });
        } else if (!manualRun.schedulerConfirmed) {
          setOverride({
            kind: "info",
            text: "Manager принял запрос. Ожидается подтверждение непосредственно от scheduler…",
          });
          enforceManualState();
        }
        return;
      }

      manualRun.schedulerConfirmed = true;
      const presentation = schedulerRunMessage(result);
      if (presentation.terminal) {
        finishManualRun(presentation);
      } else {
        setOverride(presentation);
        enforceManualState();
      }
    } catch (error) {
      if (manualRun.active) {
        finishManualRun({ kind: "error", text: error.message });
      }
    }
  };
  client.connect();
  renderClock();
}
