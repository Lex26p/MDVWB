import { TinyMqttClient } from "../mdvwb/mqtt-client.js";
import {
  alarmLabel,
  applyFanControl,
  backgroundUrl,
  binaryLabel,
  buildGroupCommandPlan,
  canSendFanCommands,
  clampScale,
  computeFitScale,
  createFanState,
  dashboardSelectionFromPayload,
  fanCommandMatchesState,
  fanCommandTopic,
  fanDeviceKey,
  fanDeviceName,
  markerMatchesFilter,
  modeLabel,
  normalizeFanCommand,
  parseFanControlTopic,
  speedLabel,
  statusClass,
  statusLabel,
  summarizeDashboard,
  temperatureLabel,
} from "./model.js";
import { emptyDashboardConfiguration } from "../mdvwb/dashboard-model.js";
import {
  cloneSchedule,
  createSchedule,
  emptySchedulesConfiguration,
  nextRunLabel,
  nextScheduleIdentifier,
  normalizeSchedule,
  normalizeSchedulesConfiguration,
  parseScheduleResultTopic,
  runStateLabel,
  scheduleTargetKey,
  scheduleTimingLabel,
  schedulesConfigurationToJson,
  schedulesForPanel,
  setScheduleTarget,
  targetSet,
} from "./schedule-model.js";

const COMMAND_CONFIRM_TIMEOUT_MS = 10000;
const DEMO_CONFIRM_DELAY_MS = 550;
const REQUESTED_PANEL_ID = new URLSearchParams(window.location.search).get("panel") || "";

const elements = {
  title: document.getElementById("dashboardTitle"),
  subtitle: document.getElementById("dashboardSubtitle"),
  connectionBadge: document.getElementById("connectionBadge"),
  detailsPanel: document.getElementById("detailsPanel"),
  detailsCloseButton: document.getElementById("detailsCloseButton"),
  scheduleButton: document.getElementById("scheduleButton"),
  schedulePanel: document.getElementById("schedulePanel"),
  scheduleCloseButton: document.getElementById("scheduleCloseButton"),
  scheduleListView: document.getElementById("scheduleListView"),
  scheduleEditorView: document.getElementById("scheduleEditorView"),
  schedulerStatusBadge: document.getElementById("schedulerStatusBadge"),
  scheduleNewButton: document.getElementById("scheduleNewButton"),
  scheduleListFeedback: document.getElementById("scheduleListFeedback"),
  scheduleList: document.getElementById("scheduleList"),
  scheduleEmpty: document.getElementById("scheduleEmpty"),
  scheduleBackButton: document.getElementById("scheduleBackButton"),
  scheduleDuplicateButton: document.getElementById("scheduleDuplicateButton"),
  scheduleNameInput: document.getElementById("scheduleNameInput"),
  scheduleEnabledInput: document.getElementById("scheduleEnabledInput"),
  scheduleKindInput: document.getElementById("scheduleKindInput"),
  scheduleTimeInput: document.getElementById("scheduleTimeInput"),
  scheduleDaysField: document.getElementById("scheduleDaysField"),
  scheduleDateField: document.getElementById("scheduleDateField"),
  scheduleDateInput: document.getElementById("scheduleDateInput"),
  scheduleTargetCount: document.getElementById("scheduleTargetCount"),
  scheduleTargetList: document.getElementById("scheduleTargetList"),
  scheduleSelectAllButton: document.getElementById("scheduleSelectAllButton"),
  scheduleClearTargetsButton: document.getElementById("scheduleClearTargetsButton"),
  schedulePowerEnabled: document.getElementById("schedulePowerEnabled"),
  schedulePowerValue: document.getElementById("schedulePowerValue"),
  scheduleModeEnabled: document.getElementById("scheduleModeEnabled"),
  scheduleModeValue: document.getElementById("scheduleModeValue"),
  scheduleSpeedEnabled: document.getElementById("scheduleSpeedEnabled"),
  scheduleSpeedValue: document.getElementById("scheduleSpeedValue"),
  scheduleSetTempEnabled: document.getElementById("scheduleSetTempEnabled"),
  scheduleSetTempValue: document.getElementById("scheduleSetTempValue"),
  scheduleNextRun: document.getElementById("scheduleNextRun"),
  scheduleEditorFeedback: document.getElementById("scheduleEditorFeedback"),
  scheduleRunButton: document.getElementById("scheduleRunButton"),
  scheduleSaveButton: document.getElementById("scheduleSaveButton"),
  scheduleDeleteButton: document.getElementById("scheduleDeleteButton"),
  drawerBackdrop: document.getElementById("drawerBackdrop"),
  notice: document.getElementById("notice"),
  placedCount: document.getElementById("placedCount"),
  visibleCount: document.getElementById("visibleCount"),
  onlineCount: document.getElementById("onlineCount"),
  alarmCount: document.getElementById("alarmCount"),
  offlineCount: document.getElementById("offlineCount"),
  waitingCount: document.getElementById("waitingCount"),
  zoomOutButton: document.getElementById("zoomOutButton"),
  zoomInButton: document.getElementById("zoomInButton"),
  fitButton: document.getElementById("fitButton"),
  mapViewport: document.getElementById("mapViewport"),
  mapSizer: document.getElementById("mapSizer"),
  mapCanvas: document.getElementById("mapCanvas"),
  backgroundImage: document.getElementById("backgroundImage"),
  mapPlaceholder: document.getElementById("mapPlaceholder"),
  markerLayer: document.getElementById("markerLayer"),
  markerTooltip: document.getElementById("markerTooltip"),
  detailsEmpty: document.getElementById("detailsEmpty"),
  detailsContent: document.getElementById("detailsContent"),
  detailsDevice: document.getElementById("detailsDevice"),
  detailsLabel: document.getElementById("detailsLabel"),
  detailsStatusBadge: document.getElementById("detailsStatusBadge"),
  detailsTemp: document.getElementById("detailsTemp"),
  detailsSetTemp: document.getElementById("detailsSetTemp"),
  detailsPower: document.getElementById("detailsPower"),
  detailsMode: document.getElementById("detailsMode"),
  detailsSpeed: document.getElementById("detailsSpeed"),
  detailsAlarm: document.getElementById("detailsAlarm"),
  detailsUpdated: document.getElementById("detailsUpdated"),
  controlAvailability: document.getElementById("controlAvailability"),
  powerCommandState: document.getElementById("powerCommandState"),
  modeCommandState: document.getElementById("modeCommandState"),
  speedCommandState: document.getElementById("speedCommandState"),
  setTempCommandState: document.getElementById("setTempCommandState"),
  modeCommand: document.getElementById("modeCommand"),
  modeApplyButton: document.getElementById("modeApplyButton"),
  speedCommand: document.getElementById("speedCommand"),
  speedApplyButton: document.getElementById("speedApplyButton"),
  setTempCommand: document.getElementById("setTempCommand"),
  setTempMinusButton: document.getElementById("setTempMinusButton"),
  setTempPlusButton: document.getElementById("setTempPlusButton"),
  setTempApplyButton: document.getElementById("setTempApplyButton"),
  commandFeedback: document.getElementById("commandFeedback"),
  groupModeButton: document.getElementById("groupModeButton"),
  groupPanel: document.getElementById("groupPanel"),
  groupSelectedCount: document.getElementById("groupSelectedCount"),
  groupSelectVisibleButton: document.getElementById("groupSelectVisibleButton"),
  groupClearButton: document.getElementById("groupClearButton"),
  groupCloseButton: document.getElementById("groupCloseButton"),
  groupPowerEnabled: document.getElementById("groupPowerEnabled"),
  groupPowerValue: document.getElementById("groupPowerValue"),
  groupModeEnabled: document.getElementById("groupModeEnabled"),
  groupModeValue: document.getElementById("groupModeValue"),
  groupSpeedEnabled: document.getElementById("groupSpeedEnabled"),
  groupSpeedValue: document.getElementById("groupSpeedValue"),
  groupSetTempEnabled: document.getElementById("groupSetTempEnabled"),
  groupSetTempValue: document.getElementById("groupSetTempValue"),
  groupFeedback: document.getElementById("groupFeedback"),
  groupApplyButton: document.getElementById("groupApplyButton"),
};

const state = {
  dashboard: emptyDashboardConfiguration(),
  dashboardCollection: null,
  panelId: REQUESTED_PANEL_ID || "main",
  dashboardStatus: null,
  receivedDashboard: false,
  connected: false,
  states: new Map(),
  selectedKey: "",
  filter: "all",
  viewScale: 1,
  followConfiguredFit: true,
  imageUrl: "",
  imageLoaded: false,
  imageFailed: false,
  demo: false,
  client: null,
  pendingCommands: new Map(),
  controlDeviceKey: "",
  groupMode: false,
  groupSelected: new Set(),
  groupOperation: null,
  groupSequence: 0,
  scheduleOpen: false,
  schedules: emptySchedulesConfiguration(),
  receivedSchedules: false,
  scheduleStatus: null,
  schedulerStatus: null,
  scheduleResults: new Map(),
  scheduleDraft: null,
  scheduleDraftPersisted: false,
  scheduleDirty: false,
  scheduleSaving: false,
  scheduleFeedback: "",
  scheduleFeedbackKind: "info",
  viewInitialized: false,
};

function showNotice(message, kind = "info") {
  elements.notice.textContent = message;
  elements.notice.className = `notice notice-${kind}`;
}

function hideNotice() {
  elements.notice.className = "notice notice-hidden";
}

function setCommandFeedback(message, kind = "info") {
  elements.commandFeedback.textContent = message;
  elements.commandFeedback.className = `command-feedback command-feedback-${kind}`;
}

function setGroupFeedback(message, kind = "info") {
  elements.groupFeedback.textContent = message;
  elements.groupFeedback.className = `group-feedback group-feedback-${kind}`;
}

function setScheduleListFeedback(message, kind = "info") {
  elements.scheduleListFeedback.textContent = message;
  elements.scheduleListFeedback.className = `schedule-feedback schedule-feedback-${kind}`;
}

function setScheduleEditorFeedback(message, kind = "info") {
  state.scheduleFeedback = String(message || "");
  state.scheduleFeedbackKind = kind;
  elements.scheduleEditorFeedback.textContent = state.scheduleFeedback;
  elements.scheduleEditorFeedback.className = `schedule-feedback schedule-feedback-${kind}`;
}

function panelSchedules() {
  return state.schedules.schedules.filter((schedule) => schedule.panelId === state.panelId);
}

function visibleScheduleFans() {
  return state.dashboard.fans.filter((fan) => fan.visible);
}

function scheduleDraftTargets() {
  return targetSet(state.scheduleDraft || { targets: [] });
}

function scheduleResultFor(id) {
  return state.scheduleResults.get(String(id)) || null;
}

function renderSchedulerStatus() {
  const schedulerState = state.demo ? "ready" : String(state.schedulerStatus?.state || "offline");
  const ready = schedulerState === "ready";
  const busy = schedulerState === "executing";
  elements.schedulerStatusBadge.className = `schedule-service-badge ${ready ? "schedule-service-ready" : busy ? "schedule-service-busy" : "schedule-service-offline"}`;
  elements.schedulerStatusBadge.innerHTML = `<span class="status-dot"></span>${ready ? "Служба работает" : busy ? "Выполняется расписание" : state.connected ? "Служба недоступна" : "MQTT отключён"}`;
}

function renderScheduleList() {
  renderSchedulerStatus();
  elements.scheduleList.textContent = "";
  const schedules = panelSchedules();
  elements.scheduleNewButton.disabled = !state.demo && (!state.connected || !state.receivedSchedules);
  elements.scheduleEmpty.classList.toggle("schedule-hidden", schedules.length > 0);
  if (!state.receivedSchedules && !state.demo) {
    setScheduleListFeedback("Загрузка расписаний…", "info");
  } else if (!state.connected && !state.demo) {
    setScheduleListFeedback("MQTT отключён. Показаны последние полученные данные.", "warning");
  } else {
    setScheduleListFeedback(`${schedules.length} расписаний для панели «${state.dashboard.title || state.panelId}».`, "info");
  }

  const fragment = document.createDocumentFragment();
  schedules.forEach((schedule) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = `schedule-list-item${schedule.enabled ? " schedule-list-item-enabled" : ""}`;
    const status = document.createElement("span");
    status.className = "schedule-list-status";
    const copy = document.createElement("span");
    copy.className = "schedule-list-copy";
    const title = document.createElement("strong");
    title.textContent = schedule.name;
    const timing = document.createElement("span");
    timing.textContent = scheduleTimingLabel(schedule);
    const detail = document.createElement("small");
    const run = scheduleResultFor(schedule.id);
    detail.textContent = run ? `${runStateLabel(run.state)} · ${schedule.targets.length} фанкойлов` : `${nextRunLabel(schedule)} · ${schedule.targets.length} фанкойлов`;
    copy.append(title, timing, detail);
    const arrow = document.createElement("span");
    arrow.className = "schedule-list-arrow";
    arrow.textContent = "›";
    button.append(status, copy, arrow);
    button.addEventListener("click", () => openScheduleEditor(schedule, true));
    fragment.appendChild(button);
  });
  elements.scheduleList.appendChild(fragment);
}

function setScheduleActionAvailability() {
  elements.schedulePowerValue.disabled = !elements.schedulePowerEnabled.checked;
  elements.scheduleModeValue.disabled = !elements.scheduleModeEnabled.checked;
  elements.scheduleSpeedValue.disabled = !elements.scheduleSpeedEnabled.checked;
  elements.scheduleSetTempValue.disabled = !elements.scheduleSetTempEnabled.checked;
}

function updateScheduleDraftFromInputs() {
  if (!state.scheduleDraft) {
    return;
  }
  state.scheduleDraft.name = elements.scheduleNameInput.value;
  state.scheduleDraft.enabled = elements.scheduleEnabledInput.checked;
  state.scheduleDraft.kind = elements.scheduleKindInput.value;
  state.scheduleDraft.time = elements.scheduleTimeInput.value;
  state.scheduleDraft.days = [...document.querySelectorAll("[data-schedule-day]:checked")].map((input) => Number(input.dataset.scheduleDay));
  state.scheduleDraft.date = elements.scheduleDateInput.value;
  const actions = {};
  if (elements.schedulePowerEnabled.checked) actions.power = elements.schedulePowerValue.value === "true";
  if (elements.scheduleModeEnabled.checked) actions.mode = Number(elements.scheduleModeValue.value);
  if (elements.scheduleSpeedEnabled.checked) actions.speed = Number(elements.scheduleSpeedValue.value);
  if (elements.scheduleSetTempEnabled.checked) actions.setTemp = Number(elements.scheduleSetTempValue.value);
  state.scheduleDraft.actions = actions;
  state.scheduleDirty = true;
  elements.scheduleDaysField.classList.toggle("schedule-hidden", state.scheduleDraft.kind !== "weekly");
  elements.scheduleDateField.classList.toggle("schedule-hidden", state.scheduleDraft.kind !== "once");
  setScheduleActionAvailability();
  try {
    elements.scheduleNextRun.textContent = nextRunLabel({ ...state.scheduleDraft, actions: Object.keys(actions).length ? actions : { power: true } });
  } catch (_error) {
    elements.scheduleNextRun.textContent = "Следующий запуск не определён";
  }
  renderMarkers();
  updateScheduleEditorButtons();
}

function updateScheduleEditorButtons() {
  const hasDraft = Boolean(state.scheduleDraft);
  const connected = state.demo || state.connected;
  const schedulerAvailable = state.demo || ["ready", "executing", "warning"].includes(String(state.schedulerStatus?.state || ""));
  elements.scheduleSaveButton.disabled = !hasDraft || !connected || state.scheduleSaving;
  elements.scheduleDeleteButton.disabled = !hasDraft || state.scheduleSaving || (state.scheduleDraftPersisted && !connected);
  elements.scheduleDuplicateButton.disabled = !hasDraft || state.scheduleSaving;
  elements.scheduleRunButton.disabled = !hasDraft || !state.scheduleDraftPersisted || state.scheduleDirty || !connected || !schedulerAvailable || state.scheduleSaving;
}

function renderScheduleTargetList() {
  const draft = state.scheduleDraft;
  elements.scheduleTargetList.textContent = "";
  const selected = draft ? scheduleDraftTargets() : new Set();
  elements.scheduleTargetCount.textContent = String(selected.size);
  const fans = visibleScheduleFans().filter((fan) => selected.has(scheduleTargetKey(fan.bus, fan.address)));
  if (!fans.length) {
    const empty = document.createElement("span");
    empty.className = "schedule-target-empty";
    empty.textContent = "Фанкойлы не выбраны";
    elements.scheduleTargetList.appendChild(empty);
    return;
  }
  fans.sort((left, right) => Number(left.number) - Number(right.number));
  fans.forEach((fan) => {
    const chip = document.createElement("span");
    chip.className = "schedule-target-chip";
    chip.textContent = `№${fan.number} ${fan.label}`;
    elements.scheduleTargetList.appendChild(chip);
  });
}

function renderScheduleEditor() {
  const draft = state.scheduleDraft;
  elements.scheduleListView.classList.toggle("schedule-hidden", Boolean(draft));
  elements.scheduleEditorView.classList.toggle("schedule-hidden", !draft);
  if (!draft) {
    renderScheduleList();
    renderMarkers();
    return;
  }

  elements.scheduleNameInput.value = draft.name || "";
  elements.scheduleEnabledInput.checked = draft.enabled !== false;
  elements.scheduleKindInput.value = draft.kind || "weekly";
  elements.scheduleTimeInput.value = draft.time || "00:00";
  elements.scheduleDateInput.value = draft.date || "";
  document.querySelectorAll("[data-schedule-day]").forEach((input) => {
    input.checked = (draft.days || []).includes(Number(input.dataset.scheduleDay));
  });
  elements.scheduleDaysField.classList.toggle("schedule-hidden", draft.kind !== "weekly");
  elements.scheduleDateField.classList.toggle("schedule-hidden", draft.kind !== "once");
  elements.schedulePowerEnabled.checked = Object.hasOwn(draft.actions || {}, "power");
  elements.schedulePowerValue.value = String(draft.actions?.power ?? true);
  elements.scheduleModeEnabled.checked = Object.hasOwn(draft.actions || {}, "mode");
  elements.scheduleModeValue.value = String(draft.actions?.mode ?? 0);
  elements.scheduleSpeedEnabled.checked = Object.hasOwn(draft.actions || {}, "speed");
  elements.scheduleSpeedValue.value = String(draft.actions?.speed ?? 4);
  elements.scheduleSetTempEnabled.checked = Object.hasOwn(draft.actions || {}, "setTemp");
  elements.scheduleSetTempValue.value = String(draft.actions?.setTemp ?? 24);
  setScheduleActionAvailability();
  elements.scheduleNextRun.textContent = nextRunLabel(draft);
  if (!state.scheduleFeedback) {
    setScheduleEditorFeedback("Нажимайте фанкойлы на карте, затем сохраните расписание.", "info");
  } else {
    setScheduleEditorFeedback(state.scheduleFeedback, state.scheduleFeedbackKind);
  }
  renderScheduleTargetList();
  updateScheduleEditorButtons();
  renderMarkers();
}

function renderSchedulePanel() {
  if (!state.scheduleOpen) {
    return;
  }
  renderScheduleEditor();
}

function openScheduleEditor(schedule, persisted) {
  state.scheduleDraft = cloneSchedule(schedule);
  state.scheduleDraft.panelId = state.panelId;
  state.scheduleDraftPersisted = Boolean(persisted);
  state.scheduleDirty = false;
  state.scheduleSaving = false;
  state.scheduleFeedback = "";
  state.selectedKey = "";
  elements.detailsPanel.classList.add("drawer-hidden");
  renderScheduleEditor();
}

function openNewSchedule() {
  const draft = createSchedule(state.panelId, state.schedules);
  openScheduleEditor(draft, false);
}

function returnToScheduleList() {
  state.scheduleDraft = null;
  state.scheduleDraftPersisted = false;
  state.scheduleDirty = false;
  state.scheduleSaving = false;
  state.scheduleFeedback = "";
  renderSchedulePanel();
}

function duplicateCurrentSchedule() {
  if (!state.scheduleDraft) {
    return;
  }
  const copy = cloneSchedule(state.scheduleDraft);
  copy.id = nextScheduleIdentifier(state.schedules);
  copy.name = `${copy.name} — копия`;
  openScheduleEditor(copy, false);
}

function scheduleConfigurationWithDraft(deleteDraft = false) {
  const config = {
    version: 1,
    revision: state.schedules.revision,
    schedules: state.schedules.schedules.map(cloneSchedule),
  };
  const id = state.scheduleDraft?.id;
  config.schedules = config.schedules.filter((schedule) => schedule.id !== id);
  if (!deleteDraft && state.scheduleDraft) {
    config.schedules.push(normalizeSchedule(state.scheduleDraft));
  }
  config.schedules.sort((left, right) => left.id.localeCompare(right.id));
  return normalizeSchedulesConfiguration(config);
}

function applyDemoScheduleConfiguration(config, message) {
  state.schedules = { ...config, revision: state.schedules.revision + 1 };
  state.receivedSchedules = true;
  state.scheduleSaving = false;
  if (state.scheduleDraft) {
    const saved = state.schedules.schedules.find((schedule) => schedule.id === state.scheduleDraft.id);
    if (saved) {
      state.scheduleDraft = cloneSchedule(saved);
      state.scheduleDraftPersisted = true;
      state.scheduleDirty = false;
    } else {
      state.scheduleDraft = null;
      state.scheduleDraftPersisted = false;
    }
  }
  setScheduleEditorFeedback(message, "success");
  renderSchedulePanel();
}

function saveCurrentSchedule() {
  if (!state.scheduleDraft) {
    return;
  }
  updateScheduleDraftFromInputs();
  try {
    const config = scheduleConfigurationWithDraft(false);
    if (state.demo) {
      applyDemoScheduleConfiguration(config, "Расписание сохранено в демонстрационном режиме.");
      return;
    }
    state.client.publish("/mdvwb/schedules/config/set", schedulesConfigurationToJson(config), { retain: false });
    state.scheduleSaving = true;
    setScheduleEditorFeedback("Расписание отправлено на Wiren Board. Ожидается подтверждение сохранения.", "info");
    updateScheduleEditorButtons();
  } catch (error) {
    state.scheduleSaving = false;
    setScheduleEditorFeedback(error.message, "error");
    updateScheduleEditorButtons();
  }
}

function deleteCurrentSchedule() {
  if (!state.scheduleDraft) {
    return;
  }
  if (!state.scheduleDraftPersisted) {
    returnToScheduleList();
    return;
  }
  try {
    const config = scheduleConfigurationWithDraft(true);
    if (state.demo) {
      applyDemoScheduleConfiguration(config, "Расписание удалено в демонстрационном режиме.");
      return;
    }
    state.client.publish("/mdvwb/schedules/config/set", schedulesConfigurationToJson(config), { retain: false });
    state.scheduleSaving = true;
    setScheduleEditorFeedback("Удаление отправлено. Ожидается подтверждение.", "info");
    updateScheduleEditorButtons();
  } catch (error) {
    setScheduleEditorFeedback(error.message, "error");
  }
}

function runCurrentSchedule() {
  if (!state.scheduleDraft || !state.scheduleDraftPersisted || state.scheduleDirty) {
    setScheduleEditorFeedback("Сначала сохраните изменения расписания.", "warning");
    return;
  }
  if (state.demo) {
    const result = { success: true, scheduleId: state.scheduleDraft.id, state: "completed", source: "manual", commands: 1, confirmed: 1, message: "Demo run completed" };
    state.scheduleResults.set(state.scheduleDraft.id, result);
    setScheduleEditorFeedback("Демонстрационный запуск завершён.", "success");
    renderScheduleEditor();
    return;
  }
  try {
    state.client.publish(`/mdvwb/schedules/${state.scheduleDraft.id}/run`, "run", { retain: false });
    setScheduleEditorFeedback("Расписание поставлено в очередь. Ожидается результат службы.", "info");
  } catch (error) {
    setScheduleEditorFeedback(error.message, "error");
  }
}

function toggleScheduleTarget(bus, address) {
  if (!state.scheduleDraft) {
    return;
  }
  const key = scheduleTargetKey(bus, address);
  const selected = scheduleDraftTargets();
  setScheduleTarget(state.scheduleDraft, bus, address, !selected.has(key));
  state.scheduleDirty = true;
  renderScheduleTargetList();
  renderMarkers();
  updateScheduleEditorButtons();
}

function selectedGroupFans() {
  return state.dashboard.fans.filter((fan) => state.groupSelected.has(fanDeviceKey(fan.bus, fan.address)));
}

function visibleGroupFans() {
  return state.dashboard.fans.filter((fan) => {
    if (!fan.visible) {
      return false;
    }
    const key = fanDeviceKey(fan.bus, fan.address);
    return markerMatchesFilter(state.states.get(key), state.filter);
  });
}

function groupCommandInput() {
  return {
    Power: { enabled: elements.groupPowerEnabled.checked, value: elements.groupPowerValue.value },
    Mode: { enabled: elements.groupModeEnabled.checked, value: elements.groupModeValue.value },
    Speed: { enabled: elements.groupSpeedEnabled.checked, value: elements.groupSpeedValue.value },
    SetTemp: { enabled: elements.groupSetTempEnabled.checked, value: elements.groupSetTempValue.value },
  };
}

function groupOperationActive() {
  const operation = state.groupOperation;
  return Boolean(operation && operation.confirmed + operation.timedOut < operation.total);
}

function updateDrawerBackdrop() {
  // Боковая панель не перекрывает карту: пользователь может сразу выбрать
  // другой фанкойл или несколько фанкойлов в групповом режиме.
  elements.drawerBackdrop.classList.add("drawer-backdrop-hidden");
}

function closeDetails() {
  state.selectedKey = "";
  state.controlDeviceKey = "";
  elements.detailsPanel.classList.add("drawer-hidden");
  renderMarkers();
  updateDrawerBackdrop();
}

function setScheduleOpen(enabled) {
  state.scheduleOpen = Boolean(enabled);
  elements.scheduleButton.setAttribute("aria-expanded", String(state.scheduleOpen));
  elements.schedulePanel.classList.toggle("drawer-hidden", !state.scheduleOpen);
  if (state.scheduleOpen) {
    if (state.groupMode) {
      setGroupMode(false);
    }
    state.selectedKey = "";
    elements.detailsPanel.classList.add("drawer-hidden");
    renderSchedulePanel();
    renderMarkers();
  }
  updateDrawerBackdrop();
}

function closeAllDrawers() {
  if (state.groupMode) {
    setGroupMode(false);
  }
  state.scheduleOpen = false;
  elements.scheduleButton.setAttribute("aria-expanded", "false");
  elements.schedulePanel.classList.add("drawer-hidden");
  closeDetails();
  updateDrawerBackdrop();
}

function setGroupMode(enabled) {
  state.groupMode = Boolean(enabled);
  elements.groupModeButton.setAttribute("aria-pressed", String(state.groupMode));
  elements.groupModeButton.classList.toggle("group-mode-active", state.groupMode);
  elements.groupPanel.classList.toggle("group-panel-hidden", !state.groupMode);
  elements.groupPanel.classList.toggle("drawer-hidden", !state.groupMode);
  if (state.groupMode) {
    state.scheduleOpen = false;
    elements.scheduleButton.setAttribute("aria-expanded", "false");
    elements.schedulePanel.classList.add("drawer-hidden");
    elements.detailsPanel.classList.add("drawer-hidden");
  } else {
    state.groupSelected.clear();
  }
  renderMarkers();
  renderGroupPanel();
  updateDrawerBackdrop();
}

function updateGroupOperationFeedback() {
  const operation = state.groupOperation;
  if (!operation) {
    return;
  }
  const completed = operation.confirmed + operation.timedOut;
  if (completed < operation.total) {
    setGroupFeedback(
      `Подтверждено ${operation.confirmed} из ${operation.total} команд. Ожидается ${operation.total - completed}.`,
      "pending",
    );
    return;
  }
  if (operation.timedOut > 0) {
    setGroupFeedback(
      `Групповая команда завершена: подтверждено ${operation.confirmed}, без подтверждения ${operation.timedOut}. Пропущено устройств: ${operation.skippedDevices}${operation.failed ? `, ошибок отправки: ${operation.failed}` : ""}.`,
      "warning",
    );
  } else {
    setGroupFeedback(
      `Все ${operation.confirmed} команд подтверждены. Изменено устройств: ${operation.targetDevices}. Пропущено: ${operation.skippedDevices}${operation.failed ? `, ошибок отправки: ${operation.failed}` : ""}.`,
      "success",
    );
  }
  renderGroupPanel();
}

function renderGroupPanel() {
  if (!state.groupMode) {
    return;
  }
  const selected = selectedGroupFans();
  elements.groupSelectedCount.textContent = String(selected.length);
  elements.groupPowerValue.disabled = !elements.groupPowerEnabled.checked;
  elements.groupModeValue.disabled = !elements.groupModeEnabled.checked;
  elements.groupSpeedValue.disabled = !elements.groupSpeedEnabled.checked;
  elements.groupSetTempValue.disabled = !elements.groupSetTempEnabled.checked;
  const hasControl = [
    elements.groupPowerEnabled,
    elements.groupModeEnabled,
    elements.groupSpeedEnabled,
    elements.groupSetTempEnabled,
  ].some((input) => input.checked);
  elements.groupApplyButton.disabled = !state.connected || selected.length === 0 || !hasControl || groupOperationActive();
}

function pendingKey(deviceKey, control) {
  return `${deviceKey}:${control}`;
}

function clearPendingEntry(key) {
  const pending = state.pendingCommands.get(key);
  if (pending?.timer) {
    clearTimeout(pending.timer);
  }
  state.pendingCommands.delete(key);
}

function clearAllPending(message = "") {
  [...state.pendingCommands.keys()].forEach(clearPendingEntry);
  state.groupOperation = null;
  if (message) {
    setCommandFeedback(message, "warning");
    setGroupFeedback(message, "warning");
  }
}

function setConnection(connected) {
  state.connected = connected;
  elements.connectionBadge.className = `status-chip ${connected ? "status-online" : "status-offline"}`;
  elements.connectionBadge.innerHTML = `<span class="status-dot"></span>${connected ? "MQTT подключён" : "MQTT отключён"}`;
  if (!connected && !state.demo) {
    clearAllPending("Соединение потеряно. Неподтверждённые команды отменены.");
    showNotice("Соединение с MQTT потеряно. На плане остаются последние полученные значения.", "warning");
  } else if (state.receivedDashboard) {
    hideNotice();
  }
  renderDetails();
  renderGroupPanel();
  renderSchedulePanel();
}

function naturalSize() {
  const width = Number(state.dashboard.background.naturalWidth) || 1600;
  const height = Number(state.dashboard.background.naturalHeight) || 900;
  return { width, height };
}

function recalculateConfiguredScale() {
  const { width, height } = naturalSize();
  state.viewScale = computeFitScale(
    state.dashboard.background.fit,
    width,
    height,
    elements.mapViewport.clientWidth,
    elements.mapViewport.clientHeight,
    state.dashboard.background.defaultScale,
  );
}

function mapGeometry(scale = clampScale(state.viewScale)) {
  const { width, height } = naturalSize();
  const viewportWidth = Math.max(1, elements.mapViewport.clientWidth);
  const viewportHeight = Math.max(1, elements.mapViewport.clientHeight);
  const scaledWidth = width * scale;
  const scaledHeight = height * scale;
  const canvasLeft = viewportWidth / 2;
  const canvasTop = viewportHeight / 2;
  return {
    width,
    height,
    viewportWidth,
    viewportHeight,
    scaledWidth,
    scaledHeight,
    canvasLeft,
    canvasTop,
    sizerWidth: scaledWidth + viewportWidth,
    sizerHeight: scaledHeight + viewportHeight,
  };
}

function renderScale() {
  const scale = clampScale(state.viewScale);
  const geometry = mapGeometry(scale);
  elements.mapSizer.style.width = `${Math.round(geometry.sizerWidth)}px`;
  elements.mapSizer.style.height = `${Math.round(geometry.sizerHeight)}px`;
  elements.mapCanvas.style.left = `${Math.round(geometry.canvasLeft)}px`;
  elements.mapCanvas.style.top = `${Math.round(geometry.canvasTop)}px`;
  elements.mapCanvas.style.width = `${geometry.width}px`;
  elements.mapCanvas.style.height = `${geometry.height}px`;
  elements.mapCanvas.style.transform = `scale(${scale})`;
  elements.fitButton.textContent = `${Math.round(scale * 100)}%`;
  elements.zoomOutButton.disabled = scale <= 0.1001;
  elements.zoomInButton.disabled = scale >= 3.999;
}

function centerMap(behavior = "auto") {
  const geometry = mapGeometry();
  const left = geometry.canvasLeft + geometry.scaledWidth / 2 - geometry.viewportWidth / 2;
  const top = geometry.canvasTop + geometry.scaledHeight / 2 - geometry.viewportHeight / 2;
  elements.mapViewport.scrollTo({
    left: Math.max(0, left),
    top: Math.max(0, top),
    behavior,
  });
}

function markerNumber(fan) {
  const configured = Number(fan?.number);
  if (Number.isInteger(configured) && configured >= 1 && configured <= 200) {
    return String(configured);
  }
  const label = String(fan?.label || "").trim();
  const explicitNumber = label.match(/№\s*(\d+)/u) || label.match(/фанкойл\s*(\d+)/iu);
  if (explicitNumber) {
    return explicitNumber[1];
  }
  if (/^\d+$/.test(label)) {
    return label;
  }
  return String(fan.address);
}

function hideMarkerTooltip() {
  elements.markerTooltip.classList.add("marker-tooltip-hidden");
  elements.markerTooltip.setAttribute("aria-hidden", "true");
}

function positionMarkerTooltip(clientX, clientY) {
  const gap = 14;
  const tooltipWidth = elements.markerTooltip.offsetWidth || 240;
  const tooltipHeight = elements.markerTooltip.offsetHeight || 120;
  const left = Math.min(window.innerWidth - tooltipWidth - 10, clientX + gap);
  const top = Math.min(window.innerHeight - tooltipHeight - 10, clientY + gap);
  elements.markerTooltip.style.left = `${Math.max(10, left)}px`;
  elements.markerTooltip.style.top = `${Math.max(10, top)}px`;
}

function showMarkerTooltip(fan, fanState, clientX, clientY) {
  const title = document.createElement("strong");
  title.textContent = fan.label || `Фанкойл №${markerNumber(fan)}`;

  const device = document.createElement("span");
  device.className = "marker-tooltip-device";
  device.textContent = `Фанкойл №${markerNumber(fan)} · ${fanDeviceName(fan.bus, fan.address)}`;

  const grid = document.createElement("div");
  grid.className = "marker-tooltip-grid";
  [
    ["Режим", modeLabel(fanState?.Mode)],
    ["Температура", temperatureLabel(fanState?.Temp)],
    ["Скорость", speedLabel(fanState?.Speed)],
  ].forEach(([label, value]) => {
    const labelElement = document.createElement("span");
    labelElement.textContent = label;
    const valueElement = document.createElement("b");
    valueElement.textContent = value;
    grid.append(labelElement, valueElement);
  });

  elements.markerTooltip.replaceChildren(title, device, grid);
  elements.markerTooltip.classList.remove("marker-tooltip-hidden");
  elements.markerTooltip.setAttribute("aria-hidden", "false");
  positionMarkerTooltip(clientX, clientY);
}

function renderMarkers() {
  const fragment = document.createDocumentFragment();
  elements.markerLayer.textContent = "";

  state.dashboard.fans
    .filter((fan) => fan.visible)
    .forEach((fan) => {
      const key = fanDeviceKey(fan.bus, fan.address);
      const fanState = state.states.get(key);
      if (!markerMatchesFilter(fanState, state.filter)) {
        return;
      }

      const marker = document.createElement("button");
      const semantic = statusClass(fanState?.Status);
      const hasPending = [...state.pendingCommands.values()].some((pending) => pending.deviceKey === key);
      marker.type = "button";
      const groupSelected = state.groupSelected.has(key);
      const scheduleMode = Boolean(state.scheduleOpen && state.scheduleDraft);
      const scheduleSelected = scheduleMode && scheduleDraftTargets().has(key);
      marker.className = `fan-marker fan-marker-${semantic}${!state.groupMode && !scheduleMode && state.selectedKey === key ? " fan-marker-selected" : ""}${groupSelected ? " fan-marker-group-selected" : ""}${state.groupMode ? " fan-marker-group-mode" : ""}${scheduleSelected ? " fan-marker-schedule-selected" : ""}${scheduleMode ? " fan-marker-schedule-mode" : ""}${hasPending ? " fan-marker-pending" : ""}`;
      marker.style.left = `${fan.x * 100}%`;
      marker.style.top = `${fan.y * 100}%`;
      marker.style.setProperty("--marker-scale", fan.markerScale);
      marker.dataset.key = key;
      marker.setAttribute("aria-label", `${fan.label}: ${statusLabel(fanState?.Status)}, температура ${temperatureLabel(fanState?.Temp)}`);
      marker.innerHTML = `
        <span class="marker-number"></span>
        <span class="group-marker-check" aria-hidden="true">✓</span>`;
      marker.querySelector(".marker-number").textContent = markerNumber(fan);
      marker.addEventListener("pointerenter", (event) => showMarkerTooltip(fan, fanState, event.clientX, event.clientY));
      marker.addEventListener("pointermove", (event) => positionMarkerTooltip(event.clientX, event.clientY));
      marker.addEventListener("pointerleave", hideMarkerTooltip);
      marker.addEventListener("blur", hideMarkerTooltip);
      marker.addEventListener("click", () => {
        hideMarkerTooltip();
        if (state.scheduleOpen) {
          if (state.scheduleDraft) {
            toggleScheduleTarget(fan.bus, fan.address);
          }
          return;
        }
        if (state.groupMode) {
          if (state.groupSelected.has(key)) {
            state.groupSelected.delete(key);
          } else {
            state.groupSelected.add(key);
          }
          renderMarkers();
          renderGroupPanel();
          return;
        }
        state.selectedKey = key;
        renderMarkers();
        renderDetails();
      });
      fragment.appendChild(marker);
    });

  elements.markerLayer.appendChild(fragment);
}

function selectedFan() {
  return state.dashboard.fans.find((fan) => fanDeviceKey(fan.bus, fan.address) === state.selectedKey) || null;
}

function formatUpdated(timestamp) {
  if (!timestamp) {
    return "Данные ещё не получены";
  }
  return `Последнее обновление: ${new Date(timestamp).toLocaleTimeString("ru-RU", { hour: "2-digit", minute: "2-digit", second: "2-digit" })}`;
}

function selectedPending(control) {
  return state.pendingCommands.get(pendingKey(state.selectedKey, control)) || null;
}

function commandStateText(control, actualLabel) {
  const pending = selectedPending(control);
  return pending ? `Ожидается подтверждение: ${pending.displayValue}` : `Фактически: ${actualLabel}`;
}

function setRowPending(control, isPending) {
  const row = document.querySelector(`[data-control-row="${control}"]`);
  row?.classList.toggle("control-row-pending", isPending);
}

function setSegmentState(control, actualValue, enabled) {
  const pending = selectedPending(control);
  document.querySelectorAll(`[data-command-control="${control}"]`).forEach((button) => {
    const value = Number(button.dataset.commandValue);
    button.disabled = !enabled || Boolean(pending);
    button.classList.toggle("command-active", !pending && Number(actualValue) === value);
    button.classList.toggle("command-pending", Boolean(pending) && pending.expected === value);
  });
  setRowPending(control, Boolean(pending));
}

function renderControls(fan, fanState) {
  const key = fanDeviceKey(fan.bus, fan.address);
  const deviceChanged = state.controlDeviceKey !== key;
  state.controlDeviceKey = key;
  const available = canSendFanCommands(state.connected, fanState);
  const offline = Number(fanState.Status) === 7 || Number(fanState.Alarm) === 2;

  if (!state.connected) {
    elements.controlAvailability.textContent = "MQTT отключён";
  } else if (fanState.Status == null) {
    elements.controlAvailability.textContent = "Ожидание данных";
  } else if (offline) {
    elements.controlAvailability.textContent = "Устройство offline";
  } else {
    elements.controlAvailability.textContent = "Управление доступно";
  }
  elements.controlAvailability.className = `control-availability ${available ? "control-available" : "control-unavailable"}`;

  elements.powerCommandState.textContent = commandStateText("Power", binaryLabel(fanState.Power));
  elements.modeCommandState.textContent = commandStateText("Mode", modeLabel(fanState.Mode));
  elements.speedCommandState.textContent = commandStateText("Speed", speedLabel(fanState.Speed));
  elements.setTempCommandState.textContent = commandStateText("SetTemp", temperatureLabel(fanState.SetTemp));

  setSegmentState("Power", fanState.Power, available);

  const modePending = selectedPending("Mode");
  const speedPending = selectedPending("Speed");
  const tempPending = selectedPending("SetTemp");

  if (deviceChanged || (document.activeElement !== elements.modeCommand && !modePending)) {
    elements.modeCommand.value = String(Number.isInteger(Number(fanState.Mode)) ? fanState.Mode : 4);
  }
  if (deviceChanged || (document.activeElement !== elements.speedCommand && !speedPending)) {
    elements.speedCommand.value = String(Number.isInteger(Number(fanState.Speed)) && Number(fanState.Speed) >= 1 ? fanState.Speed : 4);
  }
  if (deviceChanged || (document.activeElement !== elements.setTempCommand && !tempPending)) {
    const actual = Number(fanState.SetTemp);
    elements.setTempCommand.value = String(Number.isInteger(actual) && actual >= 16 && actual <= 32 ? actual : 24);
  }

  elements.modeCommand.disabled = !available || Boolean(modePending);
  elements.modeApplyButton.disabled = !available || Boolean(modePending);
  elements.speedCommand.disabled = !available || Boolean(speedPending);
  elements.speedApplyButton.disabled = !available || Boolean(speedPending);
  elements.setTempCommand.disabled = !available || Boolean(tempPending);
  elements.setTempMinusButton.disabled = !available || Boolean(tempPending);
  elements.setTempPlusButton.disabled = !available || Boolean(tempPending);
  elements.setTempApplyButton.disabled = !available || Boolean(tempPending);
  setRowPending("Mode", Boolean(modePending));
  setRowPending("Speed", Boolean(speedPending));
  setRowPending("SetTemp", Boolean(tempPending));
}

function renderDetails() {
  const fan = selectedFan();
  if (!fan) {
    state.controlDeviceKey = "";
    elements.detailsEmpty.classList.add("details-hidden");
    elements.detailsContent.classList.add("details-hidden");
    elements.detailsPanel.classList.add("drawer-hidden");
    updateDrawerBackdrop();
    return;
  }

  const fanState = state.states.get(fanDeviceKey(fan.bus, fan.address)) || createFanState(fan.bus, fan.address);
  const semantic = statusClass(fanState.Status);
  state.scheduleOpen = false;
  elements.scheduleButton.setAttribute("aria-expanded", "false");
  elements.schedulePanel.classList.add("drawer-hidden");
  elements.detailsPanel.classList.remove("drawer-hidden");
  elements.detailsEmpty.classList.add("details-hidden");
  elements.detailsContent.classList.remove("details-hidden");
  elements.detailsDevice.textContent = fanDeviceName(fan.bus, fan.address);
  elements.detailsLabel.textContent = fan.label;
  const currentStatusLabel = statusLabel(fanState.Status);
  elements.detailsStatusBadge.textContent = "";
  elements.detailsStatusBadge.title = currentStatusLabel;
  elements.detailsStatusBadge.setAttribute("aria-label", currentStatusLabel);
  elements.detailsStatusBadge.className = `device-status-dot device-status-${semantic}`;
  elements.detailsTemp.textContent = temperatureLabel(fanState.Temp);
  elements.detailsSetTemp.textContent = temperatureLabel(fanState.SetTemp);
  elements.detailsPower.textContent = binaryLabel(fanState.Power);
  elements.detailsMode.textContent = modeLabel(fanState.Mode);
  elements.detailsSpeed.textContent = speedLabel(fanState.Speed);
  elements.detailsAlarm.textContent = alarmLabel(fanState);
  elements.detailsUpdated.textContent = formatUpdated(fanState.updatedAt);
  renderControls(fan, fanState);
  updateDrawerBackdrop();
}

function renderSummary() {
  const summary = summarizeDashboard(state.dashboard, state.states);
  elements.placedCount.textContent = String(summary.placed);
  elements.visibleCount.textContent = `${summary.visible} видимых`;
  elements.onlineCount.textContent = String(summary.online);
  elements.alarmCount.textContent = String(summary.alarms + summary.offline);
  elements.offlineCount.textContent = String(summary.offline);
  elements.waitingCount.textContent = `${summary.waiting} ожидают данные`;
}

function renderBackground() {
  const url = backgroundUrl(state.dashboard);
  if (url === state.imageUrl) {
    return;
  }
  state.imageUrl = url;
  state.imageLoaded = false;
  state.imageFailed = false;

  if (!url) {
    elements.backgroundImage.removeAttribute("src");
    elements.backgroundImage.classList.add("map-background-hidden");
    elements.mapPlaceholder.classList.remove("map-placeholder-hidden");
    return;
  }

  elements.backgroundImage.onload = () => {
    state.imageLoaded = true;
    state.imageFailed = false;
    elements.backgroundImage.classList.remove("map-background-hidden");
    elements.mapPlaceholder.classList.add("map-placeholder-hidden");
    if (state.followConfiguredFit) {
      recalculateConfiguredScale();
      renderScale();
      centerMap();
    }
  };
  elements.backgroundImage.onerror = () => {
    state.imageFailed = true;
    elements.backgroundImage.classList.add("map-background-hidden");
    elements.mapPlaceholder.classList.remove("map-placeholder-hidden");
    showNotice("Не удалось загрузить изображение-подложку. Проверьте файл в /var/www/fancoils/assets.", "error");
  };
  elements.backgroundImage.src = url;
}

function renderDashboard() {
  elements.title.textContent = state.dashboard.title;
  document.title = `${state.dashboard.title} — MDVWB`;
  const revision = Number(state.dashboard.revision) || 0;
  const referenceIssues = Number(state.dashboardStatus?.referenceIssues) || 0;
  elements.subtitle.textContent = `${state.dashboard.fans.length} устройств · ${state.panelId}${referenceIssues ? ` · проблемных ссылок: ${referenceIssues}` : ""}`;

  if (state.selectedKey && !selectedFan()) {
    state.selectedKey = "";
  }
  const validKeys = new Set(state.dashboard.fans.map((fan) => fanDeviceKey(fan.bus, fan.address)));
  [...state.groupSelected].forEach((key) => {
    if (!validKeys.has(key)) {
      state.groupSelected.delete(key);
    }
  });

  renderBackground();
  if (state.followConfiguredFit) {
    recalculateConfiguredScale();
  }
  renderScale();
  if (state.receivedDashboard && !state.viewInitialized) {
    centerMap();
    state.viewInitialized = true;
  }
  renderMarkers();
  renderSummary();
  renderDetails();
  renderGroupPanel();
  renderSchedulePanel();
}

function commandDisplayValue(control, value) {
  switch (control) {
    case "Power": return value === 1 ? "Включено" : "Выключено";
    case "Mode": return modeLabel(value);
    case "Speed": return speedLabel(value);
    case "SetTemp": return temperatureLabel(value);
    case "Blinds": return value === 1 ? "Открыты" : "Закрыты";
    case "Blok": return value === 1 ? "Включена" : "Снята";
    default: return String(value);
  }
}

function simulateDemoConfirmation(fan, control, value) {
  window.setTimeout(() => {
    const key = fanDeviceKey(fan.bus, fan.address);
    let next = state.states.get(key) || createFanState(fan.bus, fan.address);
    next = applyFanControl(next, control, value);
    if (control === "Power" || control === "Mode") {
      const power = control === "Power" ? value : Number(next.Power);
      const mode = control === "Mode" ? value : Number(next.Mode);
      const statuses = { 0: 1, 1: 2, 2: 3, 3: 4, 4: 5 };
      next = applyFanControl(next, "Status", power === 0 ? 0 : (statuses[mode] ?? 5));
    }
    state.states.set(key, next);
    confirmPendingCommand(key, control, next);
    renderMarkers();
    renderSummary();
    renderDetails();
  }, DEMO_CONFIRM_DELAY_MS);
}

function beginPendingCommand(fan, control, rawValue, batchId = null) {
  const deviceKey = fanDeviceKey(fan.bus, fan.address);
  const fanState = state.states.get(deviceKey) || createFanState(fan.bus, fan.address);
  if (!canSendFanCommands(state.connected, fanState)) {
    throw new Error(`${fan.label}: команда недоступна — нет актуальной связи`);
  }

  const value = normalizeFanCommand(control, rawValue);
  const topic = fanCommandTopic(fan.bus, fan.address, control);
  const key = pendingKey(deviceKey, control);
  if (state.pendingCommands.has(key)) {
    throw new Error(`${fan.label}: параметр ${control} уже ожидает подтверждения`);
  }

  const pending = {
    key,
    deviceKey,
    control,
    expected: value,
    displayValue: commandDisplayValue(control, value),
    sentAt: Date.now(),
    batchId,
    timer: null,
  };
  pending.timer = window.setTimeout(() => {
    if (state.pendingCommands.get(key) !== pending) {
      return;
    }
    state.pendingCommands.delete(key);
    if (batchId && state.groupOperation?.id === batchId) {
      state.groupOperation.timedOut += 1;
      updateGroupOperationFeedback();
    } else {
      setCommandFeedback(`${fan.label}: команда ${control} не подтверждена за 10 секунд. Фактическое состояние не изменено.`, "warning");
    }
    renderMarkers();
    renderDetails();
    renderGroupPanel();
  }, COMMAND_CONFIRM_TIMEOUT_MS);
  state.pendingCommands.set(key, pending);

  try {
    if (state.demo) {
      simulateDemoConfirmation(fan, control, value);
    } else {
      state.client.publish(topic, String(value), { retain: false });
    }
  } catch (error) {
    clearPendingEntry(key);
    throw error;
  }
  return pending;
}

function sendSelectedCommand(control, rawValue) {
  const fan = selectedFan();
  if (!fan) {
    setCommandFeedback("Сначала выберите фанкойл.", "warning");
    return;
  }
  try {
    const pending = beginPendingCommand(fan, control, rawValue);
    setCommandFeedback(`${fan.label}: команда ${control} отправлена. Ожидается фактическое подтверждение ${pending.displayValue}.`, "pending");
    renderMarkers();
    renderDetails();
  } catch (error) {
    setCommandFeedback(error.message, "error");
    renderDetails();
  }
}

function applyGroupCommands() {
  try {
    const plan = buildGroupCommandPlan(
      state.dashboard,
      state.groupSelected,
      state.states,
      state.connected,
      groupCommandInput(),
      new Set(state.pendingCommands.keys()),
    );
    if (!plan.operations.length) {
      const reason = plan.skipped.length
        ? `Нет доступных устройств. Пропущено: ${plan.skipped.length}.`
        : "Нет новых команд для отправки.";
      setGroupFeedback(reason, "warning");
      return;
    }

    const operation = {
      id: `group-${Date.now()}-${++state.groupSequence}`,
      total: 0,
      confirmed: 0,
      timedOut: 0,
      failed: 0,
      targetDevices: plan.targets.length,
      skippedDevices: plan.skipped.length,
      pendingSkipped: plan.pendingSkipped.length,
    };
    state.groupOperation = operation;

    plan.operations.forEach(({ fan, control, value }) => {
      try {
        beginPendingCommand(fan, control, value, operation.id);
        operation.total += 1;
      } catch (_error) {
        operation.failed += 1;
      }
    });
    if (!operation.total) {
      state.groupOperation = null;
      setGroupFeedback(`Не удалось отправить команды. Ошибок публикации: ${operation.failed}.`, "error");
      renderGroupPanel();
      return;
    }

    setGroupFeedback(
      `Отправлено ${operation.total} команд для ${operation.targetDevices} устройств. Ожидается фактическое подтверждение. Пропущено устройств: ${operation.skippedDevices}${operation.pendingSkipped ? `, занятых параметров: ${operation.pendingSkipped}` : ""}${operation.failed ? `, ошибок отправки: ${operation.failed}` : ""}.`,
      "pending",
    );
    renderMarkers();
    renderDetails();
    renderGroupPanel();
  } catch (error) {
    setGroupFeedback(error.message, "error");
  }
}

function confirmPendingCommand(deviceKey, control, fanState) {
  const key = pendingKey(deviceKey, control);
  const pending = state.pendingCommands.get(key);
  if (!pending || !fanCommandMatchesState(control, pending.expected, fanState)) {
    return false;
  }
  clearPendingEntry(key);
  const fan = state.dashboard.fans.find((item) => fanDeviceKey(item.bus, item.address) === deviceKey);
  if (pending.batchId && state.groupOperation?.id === pending.batchId) {
    state.groupOperation.confirmed += 1;
    updateGroupOperationFeedback();
  } else {
    setCommandFeedback(`${fan?.label || deviceKey}: подтверждено — ${control} = ${pending.displayValue}.`, "success");
  }
  renderGroupPanel();
  return true;
}

function handleMessage(topic, payload) {
  try {
    if (topic === "/mdvwb/dashboard/config") {
      const selection = dashboardSelectionFromPayload(payload, REQUESTED_PANEL_ID);
      state.dashboardCollection = selection.collection;
      state.panelId = selection.panelId;
      state.dashboard = selection.dashboard;
      state.receivedDashboard = true;
      if (!selection.requestedFound && REQUESTED_PANEL_ID) {
        showNotice(`Панель «${REQUESTED_PANEL_ID}» не найдена. Открыта панель по умолчанию «${selection.panelId}».`, "warning");
      } else if (state.connected) {
        hideNotice();
      }
      renderDashboard();
      return;
    }

    if (topic === "/mdvwb/dashboard/status") {
      state.dashboardStatus = JSON.parse(String(payload));
      renderDashboard();
      return;
    }

    if (topic === "/mdvwb/schedules/config") {
      const wasSaving = state.scheduleSaving;
      state.schedules = normalizeSchedulesConfiguration(payload);
      state.receivedSchedules = true;
      state.scheduleSaving = false;
      if (state.scheduleDraft) {
        const saved = state.schedules.schedules.find((schedule) => schedule.id === state.scheduleDraft.id);
        if (wasSaving) {
          if (saved) {
            state.scheduleDraft = cloneSchedule(saved);
            state.scheduleDraftPersisted = true;
            state.scheduleDirty = false;
            state.scheduleFeedback = "Расписание сохранено на Wiren Board.";
            state.scheduleFeedbackKind = "success";
          } else {
            state.scheduleDraft = null;
            state.scheduleDraftPersisted = false;
            state.scheduleDirty = false;
            setScheduleListFeedback("Расписание удалено.", "success");
          }
        } else if (state.scheduleDraftPersisted && !state.scheduleDirty && saved) {
          state.scheduleDraft = cloneSchedule(saved);
        }
      }
      renderSchedulePanel();
      return;
    }

    if (topic === "/mdvwb/schedules/status") {
      state.scheduleStatus = JSON.parse(String(payload));
      renderSchedulePanel();
      return;
    }

    if (topic === "/mdvwb/scheduler/status") {
      state.schedulerStatus = JSON.parse(String(payload));
      renderSchedulePanel();
      return;
    }

    if (topic === "/mdvwb/schedules/config/result") {
      const result = JSON.parse(String(payload));
      if (!result.success) {
        state.scheduleSaving = false;
        setScheduleEditorFeedback(result.message || "Не удалось сохранить расписание.", "error");
        updateScheduleEditorButtons();
      } else if (state.scheduleDraft) {
        setScheduleEditorFeedback("Конфигурация принята. Ожидается обновлённая версия.", "success");
      }
      return;
    }

    const scheduleResultId = parseScheduleResultTopic(topic);
    if (scheduleResultId) {
      const result = JSON.parse(String(payload));
      state.scheduleResults.set(scheduleResultId, result);
      if (state.scheduleDraft?.id === scheduleResultId) {
        const success = result.success === true && result.state !== "timeout";
        setScheduleEditorFeedback(`${runStateLabel(result.state)}: ${result.message || "результат получен"}`, success ? "success" : (result.state === "executing" || result.state === "queued") ? "info" : "warning");
      }
      renderSchedulePanel();
      return;
    }

    const parsed = parseFanControlTopic(topic);
    if (!parsed) {
      return;
    }
    const current = state.states.get(parsed.key) || createFanState(parsed.bus, parsed.address);
    const next = applyFanControl(current, parsed.control, payload);
    state.states.set(parsed.key, next);
    confirmPendingCommand(parsed.key, parsed.control, next);
    renderMarkers();
    renderSummary();
    if (state.selectedKey === parsed.key) {
      renderDetails();
    }
  } catch (error) {
    showNotice(error.message, "error");
  }
}

function loadDemo() {
  state.demo = true;
  state.connected = true;
  state.receivedDashboard = true;
  const demoPayload = JSON.stringify({
    version: 2,
    revision: 8,
    defaultPanel: "main",
    panels: [
      {
        id: "main",
        title: "Главный корпус — климат",
        background: { file: "", naturalWidth: 1600, naturalHeight: 900, defaultScale: 1, fit: "contain" },
        fans: [
          { id: "fan-1-1", number: 1, bus: 1, address: 1, label: "Приёмная", x: 0.18, y: 0.28, markerScale: 1, rotation: 0, visible: true },
          { id: "fan-1-3", number: 2, bus: 1, address: 3, label: "Кабинет 203", x: 0.42, y: 0.3, markerScale: 1, rotation: 0, visible: true },
          { id: "fan-2-5", number: 3, bus: 2, address: 5, label: "Переговорная", x: 0.72, y: 0.33, markerScale: 1.1, rotation: 0, visible: true },
          { id: "fan-2-18", number: 4, bus: 2, address: 18, label: "Архив", x: 0.3, y: 0.7, markerScale: 1, rotation: 0, visible: true },
          { id: "fan-3-2", number: 200, bus: 3, address: 2, label: "Серверная", x: 0.73, y: 0.72, markerScale: 1, rotation: 0, visible: true },
        ],
      },
      {
        id: "floor-2",
        title: "Второй этаж",
        background: { file: "", naturalWidth: 1400, naturalHeight: 900, defaultScale: 1, fit: "contain" },
        fans: [
          { id: "fan-1-7", number: 101, bus: 1, address: 7, label: "Кабинет 201", x: 0.28, y: 0.38, markerScale: 1, rotation: 0, visible: true },
          { id: "fan-2-20", number: 102, bus: 2, address: 20, label: "Кабинет 202", x: 0.62, y: 0.54, markerScale: 1, rotation: 0, visible: true },
        ],
      },
    ],
  });
  const selection = dashboardSelectionFromPayload(demoPayload, REQUESTED_PANEL_ID);
  state.dashboardCollection = selection.collection;
  state.panelId = selection.panelId;
  state.dashboard = selection.dashboard;
  state.schedules = normalizeSchedulesConfiguration({
    version: 1,
    revision: 3,
    schedules: [
      {
        id: "workday-start",
        name: "Начало рабочего дня",
        enabled: true,
        panelId: "main",
        kind: "weekly",
        days: [1, 2, 3, 4, 5],
        date: "",
        time: "08:00",
        targets: [{ bus: 1, address: 1 }, { bus: 1, address: 3 }, { bus: 2, address: 5 }],
        actions: { power: true, mode: 0, speed: 2, setTemp: 23 },
      },
      {
        id: "workday-stop",
        name: "Конец рабочего дня",
        enabled: true,
        panelId: "main",
        kind: "weekly",
        days: [1, 2, 3, 4, 5],
        date: "",
        time: "19:00",
        targets: [{ bus: 1, address: 1 }, { bus: 1, address: 3 }, { bus: 2, address: 5 }, { bus: 2, address: 18 }],
        actions: { power: false },
      },
      {
        id: "floor-2-morning",
        name: "Второй этаж — утро",
        enabled: true,
        panelId: "floor-2",
        kind: "weekly",
        days: [1, 2, 3, 4, 5],
        date: "",
        time: "07:45",
        targets: [{ bus: 1, address: 7 }, { bus: 2, address: 20 }],
        actions: { power: true, mode: 0, setTemp: 22 },
      },
    ],
  });
  state.receivedSchedules = true;
  state.schedulerStatus = { state: "ready", revision: 3, schedules: 3, enabled: 3, queued: 0, active: false };

  const samples = [
    [1, 1, { Power: 1, Mode: 0, Speed: 2, SetTemp: 23, Temp: 24.5, Blinds: 1, Blok: 0, Alarm: 0, AlarmCode: 0, Status: 1 }],
    [1, 3, { Power: 0, Mode: 4, Speed: 4, SetTemp: 24, Temp: 23, Blinds: 0, Blok: 0, Alarm: 0, AlarmCode: 0, Status: 0 }],
    [2, 5, { Power: 1, Mode: 1, Speed: 1, SetTemp: 25, Temp: 21.5, Blinds: 1, Blok: 0, Alarm: 0, AlarmCode: 0, Status: 2 }],
    [2, 18, { Power: 1, Mode: 0, Speed: 3, SetTemp: 22, Temp: 27, Blinds: 1, Blok: 1, Alarm: 1, AlarmCode: 9, Status: 6 }],
    [3, 2, { Power: 1, Mode: 0, Speed: 4, SetTemp: 20, Temp: null, Blinds: 0, Blok: 0, Alarm: 2, AlarmCode: 0, Status: 7 }],
    [1, 7, { Power: 1, Mode: 0, Speed: 2, SetTemp: 22, Temp: 23.5, Blinds: 0, Blok: 0, Alarm: 0, AlarmCode: 0, Status: 1 }],
    [2, 20, { Power: 1, Mode: 3, Speed: 4, SetTemp: 24, Temp: 24, Blinds: 0, Blok: 0, Alarm: 0, AlarmCode: 0, Status: 4 }],
  ];
  samples.forEach(([bus, address, values]) => {
    let fanState = createFanState(bus, address);
    Object.entries(values).forEach(([control, value]) => {
      fanState = applyFanControl(fanState, control, value, Date.now());
    });
    state.states.set(fanDeviceKey(bus, address), fanState);
  });
  setConnection(true);
  if (!selection.requestedFound && REQUESTED_PANEL_ID) {
    showNotice(`Панель «${REQUESTED_PANEL_ID}» не найдена. Открыта «${selection.panelId}».`, "warning");
  } else {
    hideNotice();
  }
  setCommandFeedback("Демонстрационный режим: команды подтверждаются локальной имитацией.", "info");
  renderDashboard();
}

elements.zoomOutButton.addEventListener("click", () => {
  const rect = elements.mapViewport.getBoundingClientRect();
  zoomAtPointer(
    state.viewScale - Math.max(0.05, state.viewScale * 0.15),
    rect.left + rect.width / 2,
    rect.top + rect.height / 2,
  );
});

elements.zoomInButton.addEventListener("click", () => {
  const rect = elements.mapViewport.getBoundingClientRect();
  zoomAtPointer(
    state.viewScale + Math.max(0.05, state.viewScale * 0.15),
    rect.left + rect.width / 2,
    rect.top + rect.height / 2,
  );
});

elements.fitButton.addEventListener("click", () => {
  state.followConfiguredFit = true;
  recalculateConfiguredScale();
  renderScale();
  centerMap("smooth");
});

function zoomAtPointer(nextScale, clientX, clientY) {
  const previousScale = clampScale(state.viewScale);
  const scale = clampScale(nextScale);
  if (Math.abs(scale - previousScale) < 0.0001) {
    return;
  }
  const rect = elements.mapViewport.getBoundingClientRect();
  const localX = clientX - rect.left;
  const localY = clientY - rect.top;
  const previousGeometry = mapGeometry(previousScale);
  const contentX = (elements.mapViewport.scrollLeft + localX - previousGeometry.canvasLeft) / previousScale;
  const contentY = (elements.mapViewport.scrollTop + localY - previousGeometry.canvasTop) / previousScale;
  state.followConfiguredFit = false;
  state.viewScale = scale;
  renderScale();
  const nextGeometry = mapGeometry(scale);
  elements.mapViewport.scrollLeft = Math.max(0, nextGeometry.canvasLeft + contentX * scale - localX);
  elements.mapViewport.scrollTop = Math.max(0, nextGeometry.canvasTop + contentY * scale - localY);
}

elements.mapViewport.addEventListener("wheel", (event) => {
  event.preventDefault();
  const direction = event.deltaY < 0 ? 1 : -1;
  const factor = direction > 0 ? 1.12 : (1 / 1.12);
  zoomAtPointer(state.viewScale * factor, event.clientX, event.clientY);
}, { passive: false });

const panState = { active: false, pointerId: null, startX: 0, startY: 0, scrollLeft: 0, scrollTop: 0 };

function finishMapPan(pointerId) {
  if (!panState.active || (pointerId != null && panState.pointerId !== pointerId)) {
    return;
  }
  panState.active = false;
  elements.mapViewport.classList.remove("map-panning");
  if (panState.pointerId != null && elements.mapViewport.hasPointerCapture?.(panState.pointerId)) {
    elements.mapViewport.releasePointerCapture(panState.pointerId);
  }
  panState.pointerId = null;
}

elements.mapViewport.addEventListener("pointerdown", (event) => {
  if (event.button !== 0 || event.target.closest("button, input, select, a, .fan-marker")) {
    return;
  }
  hideMarkerTooltip();
  panState.active = true;
  panState.pointerId = event.pointerId;
  panState.startX = event.clientX;
  panState.startY = event.clientY;
  panState.scrollLeft = elements.mapViewport.scrollLeft;
  panState.scrollTop = elements.mapViewport.scrollTop;
  elements.mapViewport.setPointerCapture?.(event.pointerId);
  elements.mapViewport.classList.add("map-panning");
  event.preventDefault();
});

elements.mapViewport.addEventListener("pointermove", (event) => {
  if (!panState.active || panState.pointerId !== event.pointerId) {
    return;
  }
  elements.mapViewport.scrollLeft = panState.scrollLeft - (event.clientX - panState.startX);
  elements.mapViewport.scrollTop = panState.scrollTop - (event.clientY - panState.startY);
});

elements.mapViewport.addEventListener("pointerup", (event) => finishMapPan(event.pointerId));
elements.mapViewport.addEventListener("pointercancel", (event) => finishMapPan(event.pointerId));
elements.mapViewport.addEventListener("lostpointercapture", (event) => finishMapPan(event.pointerId));

document.querySelectorAll("[data-command-control]").forEach((button) => {
  button.addEventListener("click", () => sendSelectedCommand(button.dataset.commandControl, button.dataset.commandValue));
});
elements.modeApplyButton.addEventListener("click", () => sendSelectedCommand("Mode", elements.modeCommand.value));
elements.speedApplyButton.addEventListener("click", () => sendSelectedCommand("Speed", elements.speedCommand.value));
elements.setTempApplyButton.addEventListener("click", () => sendSelectedCommand("SetTemp", elements.setTempCommand.value));
elements.setTempMinusButton.addEventListener("click", () => {
  elements.setTempCommand.value = String(Math.max(16, Number(elements.setTempCommand.value || 24) - 1));
});
elements.setTempPlusButton.addEventListener("click", () => {
  elements.setTempCommand.value = String(Math.min(32, Number(elements.setTempCommand.value || 24) + 1));
});
elements.setTempCommand.addEventListener("change", () => {
  const normalized = Math.min(32, Math.max(16, Math.round(Number(elements.setTempCommand.value) || 24)));
  elements.setTempCommand.value = String(normalized);
});
elements.setTempCommand.addEventListener("keydown", (event) => {
  if (event.key === "Enter") {
    event.preventDefault();
    sendSelectedCommand("SetTemp", elements.setTempCommand.value);
  }
});

elements.detailsCloseButton.addEventListener("click", closeDetails);
elements.scheduleButton.addEventListener("click", () => setScheduleOpen(!state.scheduleOpen));
elements.scheduleCloseButton.addEventListener("click", () => setScheduleOpen(false));
elements.scheduleNewButton.addEventListener("click", openNewSchedule);
elements.scheduleBackButton.addEventListener("click", returnToScheduleList);
elements.scheduleDuplicateButton.addEventListener("click", duplicateCurrentSchedule);
elements.scheduleSelectAllButton.addEventListener("click", () => {
  if (!state.scheduleDraft) return;
  visibleScheduleFans().forEach((fan) => setScheduleTarget(state.scheduleDraft, fan.bus, fan.address, true));
  state.scheduleDirty = true;
  renderScheduleTargetList();
  renderMarkers();
  updateScheduleEditorButtons();
});
elements.scheduleClearTargetsButton.addEventListener("click", () => {
  if (!state.scheduleDraft) return;
  state.scheduleDraft.targets = [];
  state.scheduleDirty = true;
  renderScheduleTargetList();
  renderMarkers();
  updateScheduleEditorButtons();
});
elements.scheduleSaveButton.addEventListener("click", saveCurrentSchedule);
elements.scheduleDeleteButton.addEventListener("click", deleteCurrentSchedule);
elements.scheduleRunButton.addEventListener("click", runCurrentSchedule);
[
  elements.scheduleNameInput,
  elements.scheduleEnabledInput,
  elements.scheduleKindInput,
  elements.scheduleTimeInput,
  elements.scheduleDateInput,
  elements.schedulePowerEnabled,
  elements.schedulePowerValue,
  elements.scheduleModeEnabled,
  elements.scheduleModeValue,
  elements.scheduleSpeedEnabled,
  elements.scheduleSpeedValue,
  elements.scheduleSetTempEnabled,
  elements.scheduleSetTempValue,
  ...document.querySelectorAll("[data-schedule-day]"),
].forEach((input) => {
  input.addEventListener(input === elements.scheduleNameInput ? "input" : "change", updateScheduleDraftFromInputs);
});
document.addEventListener("keydown", (event) => {
  if (event.key === "Escape") {
    closeAllDrawers();
  }
});

elements.groupModeButton.addEventListener("click", () => setGroupMode(!state.groupMode));
elements.groupCloseButton.addEventListener("click", () => setGroupMode(false));
elements.groupSelectVisibleButton.addEventListener("click", () => {
  visibleGroupFans().forEach((fan) => state.groupSelected.add(fanDeviceKey(fan.bus, fan.address)));
  renderMarkers();
  renderGroupPanel();
});
elements.groupClearButton.addEventListener("click", () => {
  state.groupSelected.clear();
  renderMarkers();
  renderGroupPanel();
});
[
  [elements.groupPowerEnabled, elements.groupPowerValue],
  [elements.groupModeEnabled, elements.groupModeValue],
  [elements.groupSpeedEnabled, elements.groupSpeedValue],
  [elements.groupSetTempEnabled, elements.groupSetTempValue],
].forEach(([checkbox, input]) => {
  checkbox.addEventListener("change", () => {
    input.disabled = !checkbox.checked;
    renderGroupPanel();
  });
  input.addEventListener("change", renderGroupPanel);
});
elements.groupSetTempValue.addEventListener("change", () => {
  elements.groupSetTempValue.value = String(Math.min(32, Math.max(16, Math.round(Number(elements.groupSetTempValue.value) || 24))));
});
elements.groupApplyButton.addEventListener("click", applyGroupCommands);

const resizeObserver = new ResizeObserver(() => {
  if (state.followConfiguredFit) {
    recalculateConfiguredScale();
    renderScale();
    centerMap();
  }
});
resizeObserver.observe(elements.mapViewport);

if (new URLSearchParams(window.location.search).get("demo") === "1") {
  loadDemo();
} else {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  state.client = new TinyMqttClient({
    url: `${protocol}//${window.location.host}/mqtt`,
    clientId: `mdvwb-fancoils-${Math.random().toString(16).slice(2, 10)}`,
    keepAliveSeconds: 30,
    reconnectDelayMs: 2000,
  });
  state.client.subscribe("/mdvwb/dashboard/config");
  state.client.subscribe("/mdvwb/dashboard/status");
  state.client.subscribe("/mdvwb/schedules/config");
  state.client.subscribe("/mdvwb/schedules/status");
  state.client.subscribe("/mdvwb/schedules/config/result");
  state.client.subscribe("/mdvwb/schedules/+/result");
  state.client.subscribe("/mdvwb/scheduler/status");
  state.client.subscribe("/devices/+/controls/+");
  state.client.onConnect = () => setConnection(true);
  state.client.onDisconnect = () => setConnection(false);
  state.client.onMessage = handleMessage;
  state.client.onError = (error) => showNotice(error.message, "error");
  state.client.connect();
}

renderDashboard();
