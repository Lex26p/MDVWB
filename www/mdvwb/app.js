import { TinyMqttClient } from "./mqtt-client.js";
import {
  busCommandTopic,
  busFromEditorValues,
  canRunBusCommand,
  cloneConfiguration,
  commandLabel,
  configurationToJson,
  configurationsEqual,
  discoveryLabel,
  formatAddresses,
  nextAvailableBusId,
  normalizeConfiguration,
  parseBusTopic,
  parseJsonPayload,
  serviceLabel,
} from "./model.js";

const elements = {
  connectionBadge: document.getElementById("connectionBadge"),
  busCount: document.getElementById("busCount"),
  enabledCount: document.getElementById("enabledCount"),
  runningCount: document.getElementById("runningCount"),
  managerStatus: document.getElementById("managerStatus"),
  lastUpdate: document.getElementById("lastUpdate"),
  notice: document.getElementById("notice"),
  busGrid: document.getElementById("busGrid"),
  busCardTemplate: document.getElementById("busCardTemplate"),
  draftStatus: document.getElementById("draftStatus"),
  addBusButton: document.getElementById("addBusButton"),
  resetConfigButton: document.getElementById("resetConfigButton"),
  applyConfigButton: document.getElementById("applyConfigButton"),
  configResult: document.getElementById("configResult"),
  editorPanel: document.getElementById("editorPanel"),
  editorTitle: document.getElementById("editorTitle"),
  busEditorForm: document.getElementById("busEditorForm"),
  busIdInput: document.getElementById("busIdInput"),
  busPortInput: document.getElementById("busPortInput"),
  busAddressesInput: document.getElementById("busAddressesInput"),
  busEnabledInput: document.getElementById("busEnabledInput"),
  editorError: document.getElementById("editorError"),
  closeEditorButton: document.getElementById("closeEditorButton"),
  cancelEditorButton: document.getElementById("cancelEditorButton"),
};

const state = {
  config: { version: 1, buses: [] },
  draft: { version: 1, buses: [] },
  manager: null,
  statuses: new Map(),
  busResults: new Map(),
  discoveries: new Map(),
  discoveryResults: new Map(),
  pendingBusActions: new Map(),
  receivedConfig: false,
  connected: false,
  dirty: false,
  pendingApply: false,
  editingOriginalId: null,
  demo: false,
  client: null,
};

function setConnection(connected) {
  state.connected = connected;
  if (!connected) {
    state.pendingBusActions.clear();
  }
  elements.connectionBadge.textContent = connected ? "MQTT: подключено" : "MQTT: отключено";
  elements.connectionBadge.className = `status-badge ${connected ? "status-online" : "status-offline"}`;
  if (!connected && !state.receivedConfig) {
    showNotice("Нет соединения с MQTT. Повторное подключение выполняется автоматически.", true);
  }
  updateConfigurationControls();
  render();
}

function showNotice(message, error = false) {
  elements.notice.textContent = message;
  elements.notice.className = `notice${error ? " notice-error" : ""}`;
}

function hideNotice() {
  elements.notice.className = "notice notice-hidden";
}

function showConfigResult(message, success) {
  elements.configResult.textContent = message;
  elements.configResult.className = `operation-result ${success ? "operation-success" : "operation-error"}`;
}

function hideConfigResult() {
  elements.configResult.className = "operation-result operation-result-hidden";
}

function showEditorError(message) {
  elements.editorError.textContent = message;
  elements.editorError.className = "form-error";
}

function hideEditorError() {
  elements.editorError.className = "form-error form-error-hidden";
}

function markUpdated() {
  elements.lastUpdate.textContent = `Обновлено: ${new Date().toLocaleString("ru-RU")}`;
}

function refreshDirtyState() {
  state.dirty = !configurationsEqual(state.config, state.draft);
  updateConfigurationControls();
}

function updateConfigurationControls() {
  const canEdit = state.receivedConfig || state.demo;
  elements.addBusButton.disabled = !canEdit || state.pendingApply;
  elements.resetConfigButton.disabled = !state.dirty || state.pendingApply;
  elements.applyConfigButton.disabled =
    !state.dirty || state.pendingApply || (!state.connected && !state.demo);

  if (state.pendingApply) {
    elements.draftStatus.textContent = "Конфигурация отправлена. Ожидание подтверждения менеджера…";
  } else if (state.dirty) {
    elements.draftStatus.textContent = "Есть несохранённые изменения. Управление процессами временно отключено.";
  } else if (state.receivedConfig || state.demo) {
    elements.draftStatus.textContent = "Черновик совпадает с конфигурацией контроллера.";
  } else {
    elements.draftStatus.textContent = "Ожидание текущей конфигурации…";
  }
}

function operationText(result, pendingCommand) {
  if (pendingCommand) {
    return `${commandLabel(pendingCommand)}: команда отправлена…`;
  }
  if (!result) {
    return "";
  }
  const prefix = commandLabel(result.command || "");
  const message = result.message || (result.success ? "Команда выполнена" : "Команда не выполнена");
  return prefix ? `${prefix}: ${message}` : message;
}

function render() {
  const buses = state.draft.buses;
  elements.busCount.textContent = String(buses.length);
  elements.enabledCount.textContent = String(buses.filter((bus) => bus.enabled).length);
  elements.runningCount.textContent = String(
    buses.filter((bus) => String(state.statuses.get(bus.id)?.service).toLowerCase() === "active").length,
  );

  if (state.manager) {
    const managerState = state.manager.state || state.manager.status || "online";
    const message = state.manager.message ? ` — ${state.manager.message}` : "";
    elements.managerStatus.textContent = `${managerState}${message}`;
  } else {
    elements.managerStatus.textContent = "Ожидание состояния менеджера…";
  }

  elements.busGrid.replaceChildren();

  if (buses.length === 0) {
    if (state.receivedConfig || state.demo) {
      showNotice("В конфигурации пока нет шин. Нажмите «Добавить шину».");
    }
    updateConfigurationControls();
    return;
  }

  hideNotice();
  buses.forEach((bus) => elements.busGrid.appendChild(createBusCard(bus)));
  updateConfigurationControls();
}

function createBusCard(bus) {
  const fragment = elements.busCardTemplate.content.cloneNode(true);
  const card = fragment.querySelector(".bus-card");
  const status = state.statuses.get(bus.id) || {};
  const result = state.busResults.get(bus.id) || null;
  const discovery = state.discoveries.get(bus.id) || {};
  const discoveryResult = state.discoveryResults.get(bus.id) || {};
  const pendingCommand = state.pendingBusActions.get(bus.id) || null;
  const service = String(status.service || "unknown").toLowerCase();
  const discoveryState = String(discovery.state || "idle").toLowerCase();
  const discoveryRunning = discoveryState === "running" || pendingCommand === "discovery";

  fragment.querySelector(".bus-number").textContent = `Шина ${bus.id}`;
  fragment.querySelector(".bus-title").textContent = bus.port;
  fragment.querySelector(".bus-port").textContent = bus.port;
  fragment.querySelector(".bus-addresses").textContent = formatAddresses(bus.addresses);
  fragment.querySelector(".bus-enabled").textContent = bus.enabled ? "Активна" : "Отключена";
  fragment.querySelector(".bus-autostart").textContent =
    typeof status.autostart === "boolean" ? (status.autostart ? "Включён" : "Выключен") : "Нет данных";

  const serviceBadge = fragment.querySelector(".bus-service");
  serviceBadge.textContent = serviceLabel(service);
  serviceBadge.className = `bus-service status-badge status-${service}`;

  fragment.querySelector(".bus-discovery-status").textContent = discoveryLabel(discoveryState);
  const found = Array.isArray(discoveryResult.addresses)
    ? discoveryResult.addresses
    : Array.isArray(discovery.addresses)
      ? discovery.addresses
      : [];
  const discoveryMessage = discoveryResult.message || discovery.message || "";
  const discoveryResultElement = fragment.querySelector(".bus-discovery-result");
  if (found.length > 0) {
    discoveryResultElement.textContent = `Найдены адреса: ${formatAddresses(found)}`;
  } else if (discoveryState === "completed") {
    discoveryResultElement.textContent = "Устройства не найдены";
  } else if (discoveryState === "error" || discoveryResult.success === false) {
    discoveryResultElement.textContent = discoveryMessage || "Поиск завершился ошибкой";
  } else {
    discoveryResultElement.textContent = "";
  }

  const operationElement = fragment.querySelector(".bus-operation-result");
  operationElement.textContent = operationText(result, pendingCommand && pendingCommand !== "discovery" ? pendingCommand : null);
  if (result && result.success === false) {
    operationElement.className = "bus-operation-result inline-error";
  } else if (result && result.success === true) {
    operationElement.className = "bus-operation-result inline-success";
  }

  card.dataset.busId = String(bus.id);
  fragment.querySelector(".bus-edit").dataset.busId = String(bus.id);
  fragment.querySelector(".bus-delete").dataset.busId = String(bus.id);

  fragment.querySelectorAll(".bus-command").forEach((button) => {
    const command = button.dataset.command;
    button.dataset.busId = String(bus.id);
    button.disabled = !canRunBusCommand({
      command,
      enabled: bus.enabled,
      connected: state.connected,
      demo: state.demo,
      dirty: state.dirty || state.pendingApply,
      pending: Boolean(pendingCommand),
      discoveryRunning,
    });
  });

  return fragment;
}

function openEditor(bus = null) {
  hideEditorError();
  state.editingOriginalId = bus ? bus.id : null;
  elements.editorTitle.textContent = bus ? `Изменить шину ${bus.id}` : "Новая шина";
  elements.busIdInput.value = String(bus ? bus.id : nextAvailableBusId(state.draft));
  elements.busPortInput.value = bus ? bus.port : "";
  elements.busAddressesInput.value = bus ? bus.addresses.join(", ") : "";
  elements.busEnabledInput.checked = bus ? bus.enabled : true;
  elements.editorPanel.className = "editor-panel";
  elements.busIdInput.focus();
}

function closeEditor() {
  state.editingOriginalId = null;
  elements.busEditorForm.reset();
  hideEditorError();
  elements.editorPanel.className = "editor-panel editor-panel-hidden";
}

function saveEditorToDraft() {
  const bus = busFromEditorValues({
    id: elements.busIdInput.value,
    enabled: elements.busEnabledInput.checked,
    port: elements.busPortInput.value,
    addresses: elements.busAddressesInput.value,
  });

  const buses = state.draft.buses
    .filter((item) => item.id !== state.editingOriginalId)
    .map((item) => ({ ...item, addresses: [...item.addresses] }));
  buses.push(bus);

  state.draft = normalizeConfiguration({ version: 1, buses });
  refreshDirtyState();
  closeEditor();
  hideConfigResult();
  render();
}

function deleteBus(busId) {
  const bus = state.draft.buses.find((item) => item.id === busId);
  if (!bus) {
    return;
  }
  if (!window.confirm(`Удалить шину ${bus.id} (${bus.port}) из черновика?`)) {
    return;
  }

  state.draft = normalizeConfiguration({
    version: 1,
    buses: state.draft.buses.filter((item) => item.id !== busId),
  });
  if (state.editingOriginalId === busId) {
    closeEditor();
  }
  refreshDirtyState();
  hideConfigResult();
  render();
}

function resetDraft() {
  state.draft = cloneConfiguration(state.config);
  state.pendingApply = false;
  closeEditor();
  hideConfigResult();
  refreshDirtyState();
  render();
}

function applyConfiguration() {
  try {
    const payload = configurationToJson(state.draft);
    state.pendingApply = true;
    hideConfigResult();
    updateConfigurationControls();
    render();

    if (state.demo) {
      window.setTimeout(() => {
        handleMessage("/mdvwb/config/result", JSON.stringify({
          success: true,
          saved: true,
          message: "Демонстрационная конфигурация сохранена",
          buses: state.draft.buses.length,
          enabled: state.draft.buses.filter((bus) => bus.enabled).length,
        }));
        handleMessage("/mdvwb/config", payload);
      }, 350);
      return;
    }

    state.client.publish("/mdvwb/config/set", payload, { retain: false });
  } catch (error) {
    state.pendingApply = false;
    showConfigResult(error.message, false);
    updateConfigurationControls();
    render();
  }
}

function demoStatus(busId, service) {
  const bus = state.config.buses.find((item) => item.id === busId);
  if (!bus) {
    return;
  }
  handleMessage(`/mdvwb/buses/${busId}/status`, JSON.stringify({
    id: busId,
    configured: true,
    enabled: bus.enabled,
    service,
    autostart: bus.enabled,
    port: bus.port,
    addresses: bus.addresses,
  }));
}

function runDemoBusCommand(busId, command) {
  window.setTimeout(() => {
    if (command === "discovery") {
      handleMessage(`/mdvwb/buses/${busId}/discovery/status`, JSON.stringify({
        bus: busId,
        state: "running",
        message: "Discovery is running",
      }));
      window.setTimeout(() => {
        demoStatus(busId, "inactive");
        handleMessage(`/mdvwb/buses/${busId}/discovery/result`, JSON.stringify({
          success: true,
          bus: busId,
          addresses: [1, 5, 18],
          message: "Discovery completed",
        }));
        handleMessage(`/mdvwb/buses/${busId}/discovery/status`, JSON.stringify({
          bus: busId,
          state: "completed",
          found: 3,
          message: "Discovery completed",
        }));
        handleMessage(`/mdvwb/buses/${busId}/result`, JSON.stringify({
          success: true,
          bus: busId,
          command: "discovery",
          message: "Discovery completed",
        }));
      }, 900);
      return;
    }

    const service = command === "stop" ? "inactive" : command === "status"
      ? String(state.statuses.get(busId)?.service || "inactive")
      : "active";
    demoStatus(busId, service);
    handleMessage(`/mdvwb/buses/${busId}/result`, JSON.stringify({
      success: true,
      bus: busId,
      command,
      message: {
        start: "Bus service started",
        stop: "Bus service stopped",
        restart: "Bus service restarted",
        status: "Bus status published",
      }[command] || "Command completed",
    }));
  }, 350);
}

function sendBusCommand(busId, command) {
  const bus = state.config.buses.find((item) => item.id === busId);
  if (!bus || state.dirty || state.pendingApply) {
    showNotice("Сначала сохраните или отмените изменения конфигурации.", true);
    return;
  }

  if (command === "discovery") {
    const confirmed = window.confirm(
      `Запустить поиск на шине ${busId} (${bus.port})?\n\n` +
      "Сервис этой шины будет остановлен и после поиска автоматически не запустится.",
    );
    if (!confirmed) {
      return;
    }
  }

  try {
    const topic = busCommandTopic(busId, command);
    state.pendingBusActions.set(busId, command);
    state.busResults.delete(busId);
    render();

    if (state.demo) {
      runDemoBusCommand(busId, command);
      return;
    }
    state.client.publish(topic, "1", { retain: false });
  } catch (error) {
    state.pendingBusActions.delete(busId);
    state.busResults.set(busId, { success: false, command, message: error.message });
    render();
  }
}

function handleMessage(topic, payload) {
  try {
    if (topic === "/mdvwb/config") {
      const incoming = normalizeConfiguration(parseJsonPayload(payload, "конфигурация"));
      state.config = cloneConfiguration(incoming);

      if (!state.dirty || state.pendingApply || !state.receivedConfig) {
        state.draft = cloneConfiguration(incoming);
        state.pendingApply = false;
        closeEditor();
        refreshDirtyState();
      } else {
        showConfigResult(
          "На контроллере появилась новая конфигурация. Локальный черновик сохранён; отмените изменения, чтобы загрузить её.",
          false,
        );
      }

      state.receivedConfig = true;
      markUpdated();
      render();
      return;
    }

    if (topic === "/mdvwb/config/result") {
      const result = parseJsonPayload(payload, "результат сохранения");
      const success = result.success === true;
      const message = result.message || (success ? "Конфигурация сохранена" : "Не удалось сохранить конфигурацию");
      showConfigResult(message, success);
      if (!success) {
        state.pendingApply = false;
      }
      updateConfigurationControls();
      render();
      return;
    }

    if (topic === "/mdvwb/status") {
      state.manager = parseJsonPayload(payload, "состояние менеджера");
      markUpdated();
      render();
      return;
    }

    const busTopic = parseBusTopic(topic);
    if (!busTopic) {
      return;
    }

    const data = parseJsonPayload(payload, "данные шины");
    if (busTopic.kind === "status") {
      state.statuses.set(busTopic.busId, data);
    } else if (busTopic.kind === "result") {
      state.busResults.set(busTopic.busId, data);
      state.pendingBusActions.delete(busTopic.busId);
    } else if (busTopic.kind === "discovery/status") {
      state.discoveries.set(busTopic.busId, data);
      if (String(data.state).toLowerCase() !== "running") {
        state.pendingBusActions.delete(busTopic.busId);
      }
    } else if (busTopic.kind === "discovery/result") {
      state.discoveryResults.set(busTopic.busId, data);
    }

    markUpdated();
    render();
  } catch (error) {
    showNotice(error.message, true);
  }
}

function loadDemoData() {
  state.demo = true;
  state.connected = true;
  state.config = normalizeConfiguration({
    version: 1,
    buses: [
      { id: 1, enabled: true, port: "/dev/ttyRS485-1", addresses: [1, 2, 3] },
      { id: 2, enabled: true, port: "/dev/ttyUSB0", addresses: [5, 10, 18] },
      { id: 3, enabled: false, port: "/dev/ttyUSB1", addresses: [] },
    ],
  });
  state.draft = cloneConfiguration(state.config);
  state.receivedConfig = true;
  state.manager = { state: "demo", message: "Демонстрационный режим" };
  state.statuses.set(1, { service: "active", autostart: true });
  state.statuses.set(2, { service: "inactive", autostart: true });
  state.statuses.set(3, { service: "inactive", autostart: false });
  state.discoveries.set(2, { state: "completed" });
  state.discoveryResults.set(2, { success: true, addresses: [5, 10, 18] });
  elements.connectionBadge.textContent = "MQTT: демо";
  elements.connectionBadge.className = "status-badge status-warning";
  markUpdated();
  refreshDirtyState();
  render();
}

elements.addBusButton.addEventListener("click", () => openEditor());
elements.resetConfigButton.addEventListener("click", resetDraft);
elements.applyConfigButton.addEventListener("click", applyConfiguration);
elements.closeEditorButton.addEventListener("click", closeEditor);
elements.cancelEditorButton.addEventListener("click", closeEditor);
elements.busEditorForm.addEventListener("submit", (event) => {
  event.preventDefault();
  try {
    hideEditorError();
    saveEditorToDraft();
  } catch (error) {
    showEditorError(error.message);
  }
});

elements.busGrid.addEventListener("click", (event) => {
  const commandButton = event.target.closest(".bus-command");
  if (commandButton) {
    sendBusCommand(Number(commandButton.dataset.busId), commandButton.dataset.command);
    return;
  }

  const editButton = event.target.closest(".bus-edit");
  if (editButton) {
    const busId = Number(editButton.dataset.busId);
    const bus = state.draft.buses.find((item) => item.id === busId);
    if (bus) {
      openEditor(bus);
    }
    return;
  }

  const deleteButton = event.target.closest(".bus-delete");
  if (deleteButton) {
    deleteBus(Number(deleteButton.dataset.busId));
  }
});

if (new URLSearchParams(window.location.search).get("demo") === "1") {
  loadDemoData();
} else {
  const protocol = window.location.protocol === "https:" ? "wss:" : "ws:";
  const mqttUrl = `${protocol}//${window.location.host}/mqtt`;
  state.client = new TinyMqttClient({ url: mqttUrl, keepAliveSeconds: 30, reconnectDelayMs: 2000 });

  state.client.subscribe("/mdvwb/config");
  state.client.subscribe("/mdvwb/config/result");
  state.client.subscribe("/mdvwb/status");
  state.client.subscribe("/mdvwb/buses/+/status");
  state.client.subscribe("/mdvwb/buses/+/result");
  state.client.subscribe("/mdvwb/buses/+/discovery/status");
  state.client.subscribe("/mdvwb/buses/+/discovery/result");

  state.client.onConnect = () => setConnection(true);
  state.client.onDisconnect = () => setConnection(false);
  state.client.onMessage = handleMessage;
  state.client.onError = (error) => showNotice(error.message, true);
  state.client.connect();
}
