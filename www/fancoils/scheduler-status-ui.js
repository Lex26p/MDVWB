import { TinyMqttClient } from "../mdvwb/mqtt-client.js";

const SCHEDULER_STATUS_TOPIC = "/mdvwb/scheduler/status";
const RESULT_TOPIC_PATTERN = /^\/mdvwb\/schedules\/([A-Za-z0-9_-]{1,64})\/result$/;
const RUN_TOPIC_PATTERN = /^\/mdvwb\/schedules\/([A-Za-z0-9_-]{1,64})\/run$/;
const TERMINAL_STATES = new Set(["completed", "timeout", "failed", "rejected", "missed"]);
const SCHEDULER_STALE_AFTER_SECONDS = 125;
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
  let mainClient = null;
  let heartbeatTimer = 0;
  let feedbackOverride = null;
  let overrideTimer = 0;
  let safetyTimer = 0;
  let detachMessage = () => {};
  let detachPublish = () => {};
  let detachConnection = () => {};
  const manualRun = {
    active: false,
    scheduleId: "",
    schedulerConfirmed: false,
  };

  const schedulerHeartbeatAge = () => {
    const epoch = Number(schedulerStatus?.controllerEpoch);
    if (!Number.isInteger(epoch) || epoch <= 0) {
      return Number.POSITIVE_INFINITY;
    }
    return Math.max(0, Math.floor(Date.now() / 1000) - epoch);
  };

  const schedulerFresh = () =>
    Boolean(mainClient?.connected && schedulerStatus &&
      schedulerHeartbeatAge() <= SCHEDULER_STALE_AFTER_SECONDS);

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

    const fresh = schedulerFresh();
    const baseLabel = controllerClockLabel(schedulerStatus);
    clockElement.textContent = fresh
      ? baseLabel
      : `${baseLabel} · нет свежих данных`;
    clockElement.classList.toggle("scheduler-clock-stale", !fresh);

    if (!mainClient?.connected) {
      clockElement.title = "MQTT отключён";
    } else if (!schedulerStatus) {
      clockElement.title = "Статус scheduler ещё не получен";
    } else if (!fresh) {
      const age = schedulerHeartbeatAge();
      clockElement.title = Number.isFinite(age)
        ? `Scheduler не обновлял heartbeat ${age} с`
        : "Статус scheduler не содержит heartbeat";
    } else {
      clockElement.title = String(schedulerStatus.message || "Scheduler работает");
    }
  };

  const enforceManualState = () => {
    if (manualRun.active) {
      if (!runButton.disabled) {
        runButton.disabled = true;
      }
      if (runButton.dataset.schedulerWaiting !== "true") {
        runButton.dataset.schedulerWaiting = "true";
      }
    } else if (runButton.dataset.schedulerWaiting) {
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

  const beginManualRun = (scheduleId) => {
    manualRun.active = true;
    manualRun.scheduleId = scheduleId;
    manualRun.schedulerConfirmed = false;
    setOverride({
      kind: "info",
      text: `Запрос запуска «${scheduleId}» отправлен manager. Ожидается подтверждение scheduler…`,
    });
    enforceManualState();
    window.clearTimeout(safetyTimer);
    safetyTimer = window.setTimeout(() => {
      if (!manualRun.active) {
        return;
      }
      finishManualRun({
        kind: "warning",
        text: `Подтверждение scheduler для «${scheduleId}» не получено. Проверьте MQTT и состояние службы.`,
      });
    }, 90000);
  };

  const processResult = (topic, payload) => {
    const match = RESULT_TOPIC_PATTERN.exec(String(topic));
    if (!match || !manualRun.active || match[1] !== manualRun.scheduleId) {
      return;
    }
    const result = parseObject(payload, "результат запуска расписания");
    const scheduleId = String(result.scheduleId || match[1]);
    if (scheduleId !== manualRun.scheduleId || (result.source && result.source !== "manual")) {
      return;
    }

    if (result.origin !== "scheduler") {
      if (result.success === false) {
        finishManualRun({
          kind: "error",
          text: `Manager отклонил ручной запуск: ${result.message || "причина не указана"}.`,
        });
      } else if (!manualRun.schedulerConfirmed) {
        setOverride({
          kind: "info",
          text: `Manager принял запуск «${scheduleId}». Ожидается подтверждение непосредственно от scheduler…`,
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
  };

  const attachClient = (client) => {
    if (mainClient || !String(client?.clientId || "").startsWith("mdvwb-fancoils-")) {
      return;
    }
    mainClient = client;
    detachMessage = client.addMessageListener((topic, payload) => {
      try {
        if (topic === SCHEDULER_STATUS_TOPIC) {
          schedulerStatus = parseObject(payload, "состояние scheduler");
          renderClock();
          return;
        }
        processResult(topic, payload);
      } catch (error) {
        if (manualRun.active) {
          finishManualRun({ kind: "error", text: error.message });
        }
      }
    });
    detachPublish = client.addPublishListener((topic) => {
      const match = RUN_TOPIC_PATTERN.exec(String(topic));
      if (match) {
        beginManualRun(match[1]);
      }
    });
    detachConnection = client.addConnectionListener(({ connected }) => {
      renderClock();
      if (!connected && manualRun.active) {
        finishManualRun({
          kind: "warning",
          text: `MQTT отключён во время запуска «${manualRun.scheduleId}». Результат мог быть потерян.`,
        });
      }
    });
    renderClock();
  };

  // Demo mode completes runs inside the main page and does not publish MQTT
  // results. Do not install the manual-run bridge or button observers here.
  if (demo) {
    renderClock();
    window.setInterval(renderClock, 30000);
    return;
  }

  runButton.addEventListener("click", (event) => {
    if (schedulerFresh()) {
      return;
    }
    event.preventDefault();
    event.stopImmediatePropagation();
    setOverride({
      kind: "warning",
      text: "Ручной запуск недоступен: scheduler не присылает свежий heartbeat.",
    }, 7000);
  }, { capture: true });

  const observer = new MutationObserver(enforceManualState);
  observer.observe(feedback, { childList: true, characterData: true, subtree: true });
  // Observe only the native disabled state. Watching our own
  // data-scheduler-waiting attribute can create a MutationObserver feedback loop.
  observer.observe(runButton, { attributes: true, attributeFilter: ["disabled"] });

  TinyMqttClient.observeClients(attachClient);
  heartbeatTimer = window.setInterval(renderClock, 15000);
  window.addEventListener("pagehide", () => {
    window.clearInterval(heartbeatTimer);
    detachMessage();
    detachPublish();
    detachConnection();
  }, { once: true });
  renderClock();
}
