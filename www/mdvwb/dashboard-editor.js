import { DashboardPlacementEditor } from "./dashboard-placement-editor.js";

import {
  cloneDashboardCollection,
  cloneDashboardConfiguration,
  createUploadId,
  dashboardAssetUrl,
  dashboardCollectionToJson,
  dashboardCollectionsEqual,
  dashboardConfigurationToPanel,
  dashboardPanelToConfiguration,
  emptyDashboardCollection,
  emptyDashboardConfiguration,
  emptyDashboardPanel,
  findDashboardPanel,
  fitLabel,
  formatBytes,
  nextPanelIdentifier,
  normalizeDashboardCollection,
  normalizeDashboardConfiguration,
  panelIdentifier,
  panelUserUrl,
  parseDashboardPayload,
  readImageDimensions,
  safeUploadFileName,
  sha256HexFile,
  validateBackgroundFile,
} from "./dashboard-model.js";

const CHUNK_BYTES = 48 * 1024;
const REQUESTED_EDITOR_PANEL_ID = new URLSearchParams(window.location.search).get("panel") || "";

function markerScaleFromConfiguration(configuration) {
  const candidate = Number(configuration?.fans?.[0]?.markerScale);
  return Number.isFinite(candidate) ? Math.max(0.5, Math.min(3, candidate)) : 1;
}

function applyUniformMarkerScale(configuration, markerScale) {
  const normalized = cloneDashboardConfiguration(configuration);
  const scale = Math.max(0.5, Math.min(3, Number(markerScale) || 1));
  normalized.fans = normalized.fans.map((fan) => ({ ...fan, markerScale: scale }));
  return normalized;
}

function boundedPercent(value) {
  return Math.max(0, Math.min(100, Number(value) || 0));
}

function resultMessage(data, fallback) {
  return data?.message || fallback;
}

function loadImage(url) {
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => resolve(image);
    image.onerror = () => reject(new Error("Не удалось загрузить изображение подложки"));
    image.src = url;
  });
}

function panelConfiguration(collection, panelId) {
  const panel = findDashboardPanel(collection, panelId);
  if (!panel) {
    throw new Error(`Панель «${panelId}» не найдена`);
  }
  return dashboardPanelToConfiguration(panel, collection.revision);
}

function totalFans(collection) {
  return collection.panels.reduce((total, panel) => total + panel.fans.length, 0);
}

export class DashboardEditor {
  constructor() {
    this.elements = {
      statusBadge: document.getElementById("dashboardStatusBadge"),
      toolbarTitle: document.getElementById("dashboardToolbarTitle"),
      settingsButton: document.getElementById("dashboardSettingsButton"),
      drawerBackdrop: document.getElementById("dashboardDrawerBackdrop"),
      settingsDrawer: document.getElementById("dashboardSettingsDrawer"),
      panelSelect: document.getElementById("dashboardPanelSelect"),
      newPanelButton: document.getElementById("dashboardNewPanelButton"),
      panelIdInput: document.getElementById("dashboardPanelIdInput"),
      defaultPanelInput: document.getElementById("dashboardDefaultPanelInput"),
      duplicatePanelButton: document.getElementById("dashboardDuplicatePanelButton"),
      deletePanelButton: document.getElementById("dashboardDeletePanelButton"),
      panelUrlPreview: document.getElementById("dashboardPanelUrlPreview"),
      openPanelLink: document.getElementById("dashboardOpenPanelLink"),
      backgroundSettingsMeta: document.getElementById("dashboardBackgroundSettingsMeta"),
      revision: document.getElementById("dashboardRevision"),
      fanCount: document.getElementById("dashboardFanCount"),
      referenceIssues: document.getElementById("dashboardReferenceIssues"),
      dirtyBadge: document.getElementById("dashboardDirtyBadge"),
      titleInput: document.getElementById("dashboardTitleInput"),
      fitSelect: document.getElementById("dashboardFitSelect"),
      scaleInput: document.getElementById("dashboardScaleInput"),
      scaleValue: document.getElementById("dashboardScaleValue"),
      markerScaleInput: document.getElementById("dashboardGlobalMarkerScale"),
      markerScaleValue: document.getElementById("dashboardGlobalMarkerScaleValue"),
      resetButton: document.getElementById("dashboardResetButton"),
      saveButton: document.getElementById("dashboardSaveButton"),
      configResult: document.getElementById("dashboardConfigResult"),
      fileInput: document.getElementById("backgroundFileInput"),
      dropZone: document.getElementById("backgroundDropZone"),
      selectedInfo: document.getElementById("selectedBackgroundInfo"),
      clearSelectionButton: document.getElementById("backgroundClearSelectionButton"),
      uploadButton: document.getElementById("backgroundUploadButton"),
      cancelUploadButton: document.getElementById("backgroundCancelUploadButton"),
      uploadProgressBlock: document.getElementById("backgroundUploadProgressBlock"),
      uploadProgress: document.getElementById("backgroundUploadProgress"),
      uploadText: document.getElementById("backgroundUploadText"),
      uploadPercent: document.getElementById("backgroundUploadPercent"),
      uploadResult: document.getElementById("backgroundUploadResult"),
      previewViewport: document.getElementById("dashboardPreviewViewport"),
      previewStage: document.getElementById("dashboardPreviewStage"),
      previewImage: document.getElementById("dashboardPreviewImage"),
      previewPlaceholder: document.getElementById("dashboardPreviewPlaceholder"),
      backgroundMeta: document.getElementById("dashboardBackgroundMeta"),
      previewMode: document.getElementById("dashboardPreviewMode"),
      previewViewport: document.getElementById("dashboardPreviewViewport"),
    };

    this.state = {
      collection: emptyDashboardCollection(),
      collectionDraft: emptyDashboardCollection(),
      selectedPanelId: REQUESTED_EDITOR_PANEL_ID || "main",
      config: emptyDashboardConfiguration(),
      draft: emptyDashboardConfiguration(),
      status: null,
      received: false,
      connected: false,
      demo: false,
      pendingSave: false,
      selectedFile: null,
      selectedDimensions: null,
      selectedPreviewUrl: "",
      upload: null,
      uploadStatus: null,
      uploadResult: null,
      waiters: new Set(),
      client: null,
      markerScale: 1,
      previewScale: 1,
    };

    this.placementEditor = new DashboardPlacementEditor({
      getDashboard: () => this.state.draft,
      getMarkerScale: () => this.state.markerScale,
      onFansChanged: (fans) => {
        this.state.draft.fans = fans.map((fan) => ({ ...fan, markerScale: this.state.markerScale }));
        this.elements.fanCount.textContent = String(this.state.draft.fans.filter((fan) => fan.visible).length);
        this.renderControls();
      },
    });

    this.bindEvents();
    this.render();
  }

  setBusConfiguration(configuration) {
    this.placementEditor.setBusConfiguration(configuration);
  }

  setClient(client) {
    this.state.client = client;
  }

  setConnected(connected) {
    this.state.connected = connected;
    if (!connected && this.state.upload) {
      this.rejectWaiters(new Error("Соединение MQTT было потеряно во время загрузки"));
    }
    this.render();
  }

  setDemo(enabled) {
    this.state.demo = enabled;
  }

  loadDemoData() {
    this.state.demo = true;
    this.state.connected = true;
    const demoCollection = normalizeDashboardCollection({
      version: 2,
      revision: 2,
      defaultPanel: "main",
      panels: [
        {
          id: "main",
          title: "Главный корпус — климат",
          background: { file: "", naturalWidth: 0, naturalHeight: 0, defaultScale: 1, fit: "contain" },
          fans: [
            { id: "fan-1-1", number: 1, bus: 1, address: 1, label: "Приёмная", x: 0.28, y: 0.34, markerScale: 1, rotation: 0, visible: true },
            { id: "fan-2-18", number: 2, bus: 2, address: 18, label: "Переговорная", x: 0.67, y: 0.58, markerScale: 1, rotation: 0, visible: true },
            { id: "fan-3-2", number: 3, bus: 3, address: 2, label: "Склад", x: 0.5, y: 0.74, markerScale: 1, rotation: 0, visible: false },
          ],
        },
        {
          id: "floor-2",
          title: "Второй этаж",
          background: { file: "", naturalWidth: 0, naturalHeight: 0, defaultScale: 1, fit: "contain" },
          fans: [
            { id: "fan-1-3", number: 101, bus: 1, address: 3, label: "Кабинет 201", x: 0.36, y: 0.42, markerScale: 1, rotation: 0, visible: true },
            { id: "fan-2-20", number: 102, bus: 2, address: 20, label: "Кабинет 202", x: 0.64, y: 0.55, markerScale: 1, rotation: 0, visible: true },
          ],
        },
      ],
    });
    this.applyIncomingCollection(demoCollection, { force: true });
    this.state.status = {
      state: "ready",
      revision: demoCollection.revision,
      panels: demoCollection.panels.length,
      fans: totalFans(demoCollection),
      referenceIssues: 0,
    };
    this.state.received = true;
    this.render();
  }


  buildDraftCollection() {
    const collection = cloneDashboardCollection(this.state.collectionDraft);
    const index = collection.panels.findIndex((panel) => panel.id === this.state.selectedPanelId);
    if (index < 0) {
      throw new Error("Выбранная панель отсутствует в черновике");
    }
    collection.panels[index] = dashboardConfigurationToPanel(
      applyUniformMarkerScale(this.state.draft, this.state.markerScale),
      this.state.selectedPanelId,
    );
    collection.revision = this.state.collection.revision;
    return normalizeDashboardCollection(collection);
  }

  commitCurrentPanel() {
    this.state.collectionDraft = this.buildDraftCollection();
  }

  applyIncomingCollection(collection, { force = false } = {}) {
    const incoming = cloneDashboardCollection(collection);
    const dirty = !force && this.state.received && this.isDirty();
    this.state.collection = cloneDashboardCollection(incoming);
    if (dirty && !this.state.pendingSave) {
      this.showConfigResult(
        "Панели были изменены в другом окне. Локальный черновик сохранён; нажмите «Отменить», чтобы загрузить новую версию.",
        false,
      );
      return;
    }

    this.state.collectionDraft = cloneDashboardCollection(incoming);
    const preferred = findDashboardPanel(incoming, this.state.selectedPanelId)
      ? this.state.selectedPanelId
      : incoming.defaultPanel;
    this.loadPanel(preferred);
    this.state.pendingSave = false;
    this.state.received = true;
    this.hideConfigResult();
  }

  loadPanel(panelId) {
    const panel = findDashboardPanel(this.state.collectionDraft, panelId);
    if (!panel) {
      throw new Error(`Панель «${panelId}» не найдена`);
    }
    this.state.selectedPanelId = panel.id;
    const editorUrl = new URL(window.location.href);
    editorUrl.searchParams.set("panel", panel.id);
    window.history.replaceState(null, "", `${editorUrl.pathname}${editorUrl.search}${editorUrl.hash}`);
    const currentPanel = findDashboardPanel(this.state.collection, panel.id) || panel;
    const incoming = dashboardPanelToConfiguration(currentPanel, this.state.collection.revision);
    const draft = dashboardPanelToConfiguration(panel, this.state.collection.revision);
    this.state.markerScale = markerScaleFromConfiguration(draft);
    this.state.config = applyUniformMarkerScale(incoming, markerScaleFromConfiguration(incoming));
    this.state.draft = applyUniformMarkerScale(draft, this.state.markerScale);
    this.state.previewScale = this.state.draft.background.defaultScale;
    this.clearSelectedFileAfterUpload();
    this.hideUploadResult();
    this.placementEditor.clearSelection?.();
  }

  switchPanel(panelId) {
    if (panelId === this.state.selectedPanelId) {
      return;
    }
    this.commitCurrentPanel();
    this.loadPanel(panelId);
    this.hideConfigResult();
    this.render();
  }

  createPanel() {
    try {
      this.commitCurrentPanel();
      const id = nextPanelIdentifier(this.state.collectionDraft);
      const panel = emptyDashboardPanel(id, `Новая панель ${this.state.collectionDraft.panels.length + 1}`);
      this.state.collectionDraft.panels.push(panel);
      this.state.selectedPanelId = id;
      this.loadPanel(id);
      this.openDrawer(this.elements.settingsDrawer);
      this.render();
      this.elements.panelIdInput.select();
    } catch (error) {
      this.showConfigResult(error.message, false);
    }
  }

  duplicatePanel() {
    try {
      this.commitCurrentPanel();
      const source = findDashboardPanel(this.state.collectionDraft, this.state.selectedPanelId);
      if (!source) {
        throw new Error("Выбранная панель не найдена");
      }
      const id = nextPanelIdentifier(this.state.collectionDraft);
      const copy = {
        ...source,
        id,
        title: `${source.title} — копия`,
        background: { ...source.background },
        fans: source.fans.map((fan) => ({ ...fan })),
      };
      this.state.collectionDraft.panels.push(copy);
      this.loadPanel(id);
      this.render();
    } catch (error) {
      this.showConfigResult(error.message, false);
    }
  }

  deletePanel() {
    try {
      this.commitCurrentPanel();
      if (this.state.collectionDraft.panels.length <= 1) {
        throw new Error("Нельзя удалить единственную пользовательскую панель");
      }
      const id = this.state.selectedPanelId;
      this.state.collectionDraft.panels = this.state.collectionDraft.panels.filter((panel) => panel.id !== id);
      if (this.state.collectionDraft.defaultPanel === id) {
        this.state.collectionDraft.defaultPanel = this.state.collectionDraft.panels[0].id;
      }
      this.loadPanel(this.state.collectionDraft.defaultPanel);
      this.closeDrawers();
      this.render();
    } catch (error) {
      this.showConfigResult(error.message, false);
    }
  }

  renameCurrentPanel(rawId) {
    try {
      const nextId = panelIdentifier(rawId);
      if (nextId === this.state.selectedPanelId) {
        return;
      }
      this.commitCurrentPanel();
      if (findDashboardPanel(this.state.collectionDraft, nextId)) {
        throw new Error(`Панель с ID «${nextId}» уже существует`);
      }
      const panel = findDashboardPanel(this.state.collectionDraft, this.state.selectedPanelId);
      if (!panel) {
        throw new Error("Выбранная панель не найдена");
      }
      const previous = panel.id;
      panel.id = nextId;
      if (this.state.collectionDraft.defaultPanel === previous) {
        this.state.collectionDraft.defaultPanel = nextId;
      }
      this.state.selectedPanelId = nextId;
      this.loadPanel(nextId);
      this.render();
    } catch (error) {
      this.elements.panelIdInput.value = this.state.selectedPanelId;
      this.showConfigResult(error.message, false);
    }
  }

  handleMessage(topic, payload) {
    try {
      if (topic === "/mdvwb/dashboard/config") {
        const incoming = normalizeDashboardCollection(
          parseDashboardPayload(payload, "конфигурацию веб-панелей"),
        );
        this.applyIncomingCollection(incoming);
        this.render();
        return true;
      }

      if (topic === "/mdvwb/dashboard/config/result") {
        const result = parseDashboardPayload(payload, "результат сохранения панели");
        this.showConfigResult(
          resultMessage(result, result.success ? "Панель сохранена" : "Не удалось сохранить панель"),
          result.success === true,
        );
        if (result.success !== true) {
          this.state.pendingSave = false;
        }
        this.render();
        return true;
      }

      if (topic === "/mdvwb/dashboard/status") {
        this.state.status = parseDashboardPayload(payload, "состояние панели");
        this.render();
        return true;
      }

      if (topic === "/mdvwb/dashboard/background/upload/status") {
        const status = parseDashboardPayload(payload, "состояние загрузки подложки");
        this.state.uploadStatus = status;
        this.notifyWaiters("status", status);
        this.renderUploadProgress(status);
        this.render();
        return true;
      }

      if (topic === "/mdvwb/dashboard/background/upload/result") {
        const result = parseDashboardPayload(payload, "результат загрузки подложки");
        this.state.uploadResult = result;
        this.notifyWaiters("result", result);
        if (result.success === false) {
          this.showUploadResult(resultMessage(result, "Не удалось загрузить изображение"), false);
        }
        this.render();
        return true;
      }
    } catch (error) {
      this.showConfigResult(error.message, false);
      this.render();
      return true;
    }
    return false;
  }

  bindEvents() {
    this.elements.titleInput.addEventListener("input", () => {
      this.state.draft.title = this.elements.titleInput.value;
      this.elements.toolbarTitle.textContent = this.state.draft.title || "Панель фанкойлов";
      const option = this.elements.panelSelect.querySelector(`option[value="${CSS.escape(this.state.selectedPanelId)}"]`);
      if (option) {
        option.textContent = `${this.state.draft.title || "Панель фанкойлов"} · ${this.state.selectedPanelId}`;
      }
      this.renderControls();
    });

    this.elements.fitSelect.addEventListener("change", () => {
      this.state.draft.background.fit = this.elements.fitSelect.value;
      this.state.previewScale = this.state.draft.background.defaultScale;
      this.renderPreview();
      this.renderControls();
    });

    this.elements.scaleInput.addEventListener("input", () => {
      this.setScale(Number(this.elements.scaleInput.value) / 100);
    });

    this.elements.markerScaleInput.addEventListener("input", () => {
      this.setMarkerScale(Number(this.elements.markerScaleInput.value) / 100);
    });

    this.elements.previewViewport.addEventListener("wheel", (event) => {
      event.preventDefault();
      const factor = event.deltaY < 0 ? 1.1 : 0.9;
      this.setPreviewScale(this.state.previewScale * factor);
    }, { passive: false });

    this.elements.resetButton.addEventListener("click", () => {
      this.state.collectionDraft = cloneDashboardCollection(this.state.collection);
      const nextPanel = findDashboardPanel(this.state.collectionDraft, this.state.selectedPanelId)
        ? this.state.selectedPanelId
        : this.state.collectionDraft.defaultPanel;
      this.loadPanel(nextPanel);
      this.state.pendingSave = false;
      this.hideConfigResult();
      this.render();
    });

    this.elements.saveButton.addEventListener("click", () => this.saveConfiguration());

    this.elements.fileInput.addEventListener("change", () => {
      const file = this.elements.fileInput.files?.[0] || null;
      if (file) {
        this.selectFile(file);
      }
    });

    ["dragenter", "dragover"].forEach((name) => {
      this.elements.dropZone.addEventListener(name, (event) => {
        event.preventDefault();
        if (!this.state.upload) {
          this.elements.dropZone.classList.add("upload-drop-zone-active");
        }
      });
    });
    ["dragleave", "drop"].forEach((name) => {
      this.elements.dropZone.addEventListener(name, (event) => {
        event.preventDefault();
        this.elements.dropZone.classList.remove("upload-drop-zone-active");
      });
    });
    this.elements.dropZone.addEventListener("drop", (event) => {
      if (this.state.upload) {
        return;
      }
      const file = event.dataTransfer?.files?.[0] || null;
      if (file) {
        this.selectFile(file);
      }
    });

    this.elements.clearSelectionButton.addEventListener("click", () => this.clearSelectedFile());
    this.elements.uploadButton.addEventListener("click", () => this.uploadSelectedFile());
    this.elements.cancelUploadButton.addEventListener("click", () => this.cancelUpload());

    this.elements.settingsButton.addEventListener("click", () => this.openDrawer(this.elements.settingsDrawer));
    this.elements.panelSelect.addEventListener("change", () => this.switchPanel(this.elements.panelSelect.value));
    this.elements.newPanelButton.addEventListener("click", () => this.createPanel());
    this.elements.duplicatePanelButton.addEventListener("click", () => this.duplicatePanel());
    this.elements.deletePanelButton.addEventListener("click", () => this.deletePanel());
    this.elements.panelIdInput.addEventListener("change", () => this.renameCurrentPanel(this.elements.panelIdInput.value));
    this.elements.defaultPanelInput.addEventListener("change", () => {
      if (this.elements.defaultPanelInput.checked) {
        this.state.collectionDraft.defaultPanel = this.state.selectedPanelId;
      } else if (this.state.collectionDraft.defaultPanel === this.state.selectedPanelId) {
        this.elements.defaultPanelInput.checked = true;
      }
      this.renderControls();
    });
    this.elements.drawerBackdrop.addEventListener("click", () => this.closeDrawers());
    document.querySelectorAll(".dashboard-drawer-close").forEach((button) => {
      button.addEventListener("click", () => this.closeDrawers());
    });
    document.addEventListener("keydown", (event) => {
      if (event.key === "Escape") {
        this.closeDrawers();
      }
    });
  }

  openDrawer(drawer) {
    this.closeDrawers();
    this.elements.drawerBackdrop.classList.remove("dashboard-drawer-hidden");
    drawer.classList.remove("dashboard-drawer-hidden");
  }

  closeDrawers() {
    this.elements.drawerBackdrop.classList.add("dashboard-drawer-hidden");
    this.elements.settingsDrawer.classList.add("dashboard-drawer-hidden");
  }

  isDirty() {
    try {
      return !dashboardCollectionsEqual(this.state.collection, this.buildDraftCollection());
    } catch (_) {
      return true;
    }
  }

  setScale(value) {
    const scale = Math.max(0.25, Math.min(4, Math.round(value * 20) / 20));
    this.state.draft.background.defaultScale = scale;
    this.state.previewScale = scale;
    this.elements.scaleInput.value = String(Math.round(scale * 100));
    this.renderPreview();
    this.renderControls();
  }

  setPreviewScale(value) {
    this.state.previewScale = Math.max(0.25, Math.min(4, Math.round(value * 100) / 100));
    this.renderPreview();
  }

  setMarkerScale(value) {
    const scale = Math.max(0.5, Math.min(3, Math.round(value * 20) / 20));
    this.state.markerScale = scale;
    this.state.draft.fans = this.state.draft.fans.map((fan) => ({ ...fan, markerScale: scale }));
    this.elements.markerScaleInput.value = String(Math.round(scale * 100));
    this.elements.markerScaleValue.textContent = `${Math.round(scale * 100)}%`;
    this.placementEditor.renderMarkers();
    this.renderControls();
  }

  saveConfiguration() {
    try {
      const submitted = this.buildDraftCollection();
      const payload = dashboardCollectionToJson(submitted);
      this.state.pendingSave = true;
      this.hideConfigResult();
      this.renderControls();

      if (this.state.demo) {
        const updated = cloneDashboardCollection(submitted);
        updated.revision += 1;
        window.setTimeout(() => {
          this.handleMessage("/mdvwb/dashboard/config/result", JSON.stringify({
            success: true,
            saved: true,
            message: "Демонстрационные панели сохранены",
            revision: updated.revision,
            panels: updated.panels.length,
            fans: totalFans(updated),
            referenceIssues: 0,
          }));
          this.handleMessage("/mdvwb/dashboard/config", JSON.stringify(updated));
          this.handleMessage("/mdvwb/dashboard/status", JSON.stringify({
            state: "ready",
            revision: updated.revision,
            panels: updated.panels.length,
            fans: totalFans(updated),
            referenceIssues: 0,
          }));
        }, 250);
        return;
      }

      this.state.client.publish("/mdvwb/dashboard/config/set", payload, { retain: false });
    } catch (error) {
      this.state.pendingSave = false;
      this.showConfigResult(error.message, false);
      this.render();
    }
  }

  async selectFile(file) {
    try {
      validateBackgroundFile(file);
      const dimensions = await readImageDimensions(file);
      this.releaseSelectedPreview();
      this.state.selectedFile = file;
      this.state.selectedDimensions = dimensions;
      this.state.selectedPreviewUrl = URL.createObjectURL(file);
      this.hideUploadResult();
      this.render();
    } catch (error) {
      this.clearSelectedFile();
      this.showUploadResult(error.message, false);
    }
  }

  clearSelectedFile() {
    if (this.state.upload) {
      return;
    }
    this.releaseSelectedPreview();
    this.state.selectedFile = null;
    this.state.selectedDimensions = null;
    this.elements.fileInput.value = "";
    this.hideUploadResult();
    this.render();
  }

  releaseSelectedPreview() {
    if (this.state.selectedPreviewUrl) {
      URL.revokeObjectURL(this.state.selectedPreviewUrl);
      this.state.selectedPreviewUrl = "";
    }
  }

  async uploadSelectedFile() {
    if (this.state.demo) {
      await this.demoUpload();
      return;
    }

    try {
      if (!this.state.connected || !this.state.client) {
        throw new Error("Нет соединения с MQTT");
      }
      if (this.isDirty()) {
        throw new Error("Сначала сохраните или отмените изменения параметров панели");
      }
      const file = validateBackgroundFile(this.state.selectedFile);
      const dimensions = this.state.selectedDimensions || await readImageDimensions(file);
      const uploadId = createUploadId();
      this.state.upload = { uploadId, file, cancelled: false, dimensions };
      this.state.uploadStatus = null;
      this.state.uploadResult = null;
      this.hideUploadResult();
      this.render();

      this.updateLocalUploadProgress(0, "Вычисление SHA-256…");
      const sha256 = await sha256HexFile(file);
      if (this.state.upload?.cancelled) {
        throw new Error("Загрузка отменена");
      }

      const startWait = this.waitForUploadEvent(
        (kind, data) => kind === "status" && data.uploadId === uploadId && data.state === "uploading",
        uploadId,
      );
      this.state.client.publish(
        "/mdvwb/dashboard/background/upload/start",
        JSON.stringify({
          version: 1,
          uploadId,
          fileName: safeUploadFileName(file.name),
          size: file.size,
          sha256,
          panelId: this.state.selectedPanelId,
          revision: this.state.collection.revision,
        }),
        { retain: false },
      );
      await startWait;

      let index = 0;
      let offset = 0;
      while (offset < file.size) {
        if (!this.state.upload || this.state.upload.cancelled) {
          throw new Error("Загрузка отменена");
        }
        const end = Math.min(offset + CHUNK_BYTES, file.size);
        const bytes = new Uint8Array(await file.slice(offset, end).arrayBuffer());
        const expectedReceived = end;
        const chunkWait = this.waitForUploadEvent(
          (kind, data) => kind === "status" && data.uploadId === uploadId &&
            data.state === "uploading" && Number(data.received) >= expectedReceived,
          uploadId,
        );
        this.state.client.publish(
          `/mdvwb/dashboard/background/upload/chunk/${uploadId}/${index}`,
          bytes,
          { retain: false },
        );
        await chunkWait;
        offset = end;
        index += 1;
      }

      const finishWait = this.waitForUploadEvent(
        (kind, data) => kind === "result" && data.uploadId === uploadId && data.saved === true,
        uploadId,
        60000,
      );
      this.state.client.publish(
        `/mdvwb/dashboard/background/upload/finish/${uploadId}`,
        "1",
        { retain: false },
      );
      const result = await finishWait;
      this.showUploadResult(
        `Подложка загружена: ${result.width}×${result.height}, ${formatBytes(result.size)}`,
        true,
      );
      this.state.upload = null;
      this.clearSelectedFileAfterUpload();
      this.render();
    } catch (error) {
      const cancelled = this.state.upload?.cancelled;
      this.rejectWaiters(error);
      this.state.upload = null;
      this.showUploadResult(cancelled ? "Загрузка отменена" : error.message, false);
      this.render();
    }
  }

  async demoUpload() {
    try {
      const file = validateBackgroundFile(this.state.selectedFile);
      const dimensions = this.state.selectedDimensions || await readImageDimensions(file);
      this.state.upload = { uploadId: createUploadId(), file, cancelled: false, dimensions };
      this.render();
      for (let progress = 0; progress <= 100; progress += 10) {
        if (this.state.upload?.cancelled) {
          throw new Error("Загрузка отменена");
        }
        this.updateLocalUploadProgress(progress, progress < 100 ? "Демонстрационная загрузка…" : "Готово");
        await new Promise((resolve) => window.setTimeout(resolve, 35));
      }
      const updated = this.buildDraftCollection();
      const panel = findDashboardPanel(updated, this.state.selectedPanelId);
      panel.background.file = "";
      panel.background.naturalWidth = dimensions.width;
      panel.background.naturalHeight = dimensions.height;
      updated.revision += 1;
      this.applyIncomingCollection(updated, { force: true });
      this.state.status = {
        state: "ready",
        revision: updated.revision,
        panels: updated.panels.length,
        fans: totalFans(updated),
        referenceIssues: 0,
      };
      this.showUploadResult(`Демо-подложка подготовлена: ${dimensions.width}×${dimensions.height}`, true);
      this.state.upload = null;
      this.render();
    } catch (error) {
      this.state.upload = null;
      this.showUploadResult(error.message, false);
      this.render();
    }
  }

  clearSelectedFileAfterUpload() {
    this.releaseSelectedPreview();
    this.state.selectedFile = null;
    this.state.selectedDimensions = null;
    this.elements.fileInput.value = "";
  }

  cancelUpload() {
    if (!this.state.upload) {
      return;
    }
    const { uploadId } = this.state.upload;
    this.state.upload.cancelled = true;
    this.rejectWaiters(new Error("Загрузка отменена"));
    if (!this.state.demo && this.state.connected && this.state.client) {
      try {
        this.state.client.publish(
          `/mdvwb/dashboard/background/upload/cancel/${uploadId}`,
          "1",
          { retain: false },
        );
      } catch (_) {
      }
    }
    this.state.upload = null;
    this.showUploadResult("Загрузка отменена", false);
    this.render();
  }

  waitForUploadEvent(predicate, uploadId, timeoutMs = 30000) {
    return new Promise((resolve, reject) => {
      const waiter = { predicate, uploadId, resolve, reject, timer: null };
      waiter.timer = window.setTimeout(() => {
        this.state.waiters.delete(waiter);
        reject(new Error("Истекло время ожидания ответа менеджера загрузки"));
      }, timeoutMs);
      this.state.waiters.add(waiter);
    });
  }

  notifyWaiters(kind, data) {
    [...this.state.waiters].forEach((waiter) => {
      if (kind === "result" && data.uploadId === waiter.uploadId && data.success === false) {
        window.clearTimeout(waiter.timer);
        this.state.waiters.delete(waiter);
        waiter.reject(new Error(resultMessage(data, "Ошибка загрузки изображения")));
        return;
      }
      if (waiter.predicate(kind, data)) {
        window.clearTimeout(waiter.timer);
        this.state.waiters.delete(waiter);
        waiter.resolve(data);
      }
    });
  }

  rejectWaiters(error) {
    [...this.state.waiters].forEach((waiter) => {
      window.clearTimeout(waiter.timer);
      waiter.reject(error);
    });
    this.state.waiters.clear();
  }

  showConfigResult(message, success) {
    this.elements.configResult.textContent = message;
    this.elements.configResult.className = `operation-result ${success ? "operation-success" : "operation-error"}`;
  }

  hideConfigResult() {
    this.elements.configResult.className = "operation-result operation-result-hidden";
  }

  showUploadResult(message, success) {
    this.elements.uploadResult.textContent = message;
    this.elements.uploadResult.className = `operation-result ${success ? "operation-success" : "operation-error"}`;
  }

  hideUploadResult() {
    this.elements.uploadResult.className = "operation-result operation-result-hidden";
  }

  updateLocalUploadProgress(progress, text) {
    this.elements.uploadProgressBlock.classList.remove("upload-progress-hidden");
    this.elements.uploadProgress.value = boundedPercent(progress);
    this.elements.uploadPercent.textContent = `${Math.round(boundedPercent(progress))}%`;
    this.elements.uploadText.textContent = text;
  }

  renderUploadProgress(status) {
    if (!status || status.state === "idle") {
      if (!this.state.upload) {
        this.elements.uploadProgressBlock.classList.add("upload-progress-hidden");
      }
      return;
    }
    const progress = boundedPercent(status.progress);
    const text = status.message || {
      uploading: "Загрузка изображения…",
      completed: "Загрузка завершена",
      error: "Ошибка загрузки",
    }[status.state] || "Обработка изображения…";
    this.updateLocalUploadProgress(progress, text);
  }

  render() {
    const status = this.state.status || {};
    const stateName = String(status.state || (this.state.received ? "ready" : "offline")).toLowerCase();
    this.elements.statusBadge.textContent = {
      ready: "Панель готова",
      error: "Ошибка панели",
      offline: "Ожидание панели",
    }[stateName] || stateName;
    this.elements.statusBadge.className = `status-badge status-${stateName === "ready" ? "online" : stateName}`;
    this.elements.revision.textContent = String(this.state.collection.revision || 0);
    this.elements.fanCount.textContent = String(this.state.draft.fans.filter((fan) => fan.visible).length);
    this.elements.referenceIssues.textContent = String(status.referenceIssues ?? 0);

    this.elements.toolbarTitle.textContent = this.state.draft.title || "Панель фанкойлов";
    this.elements.panelSelect.replaceChildren(...this.state.collectionDraft.panels.map((panel) => {
      const option = document.createElement("option");
      option.value = panel.id;
      option.textContent = `${panel.title} · ${panel.id}`;
      option.selected = panel.id === this.state.selectedPanelId;
      return option;
    }));
    this.elements.panelIdInput.value = this.state.selectedPanelId;
    this.elements.defaultPanelInput.checked = this.state.collectionDraft.defaultPanel === this.state.selectedPanelId;
    const userUrl = panelUserUrl(this.state.selectedPanelId);
    this.elements.panelUrlPreview.textContent = userUrl;
    this.elements.openPanelLink.href = userUrl;
    this.elements.titleInput.value = this.state.draft.title;
    this.elements.fitSelect.value = this.state.draft.background.fit;
    this.elements.scaleInput.value = String(Math.round(this.state.draft.background.defaultScale * 100));
    this.elements.scaleValue.textContent = `${Math.round(this.state.draft.background.defaultScale * 100)}%`;
    this.elements.markerScaleInput.value = String(Math.round(this.state.markerScale * 100));
    this.elements.markerScaleValue.textContent = `${Math.round(this.state.markerScale * 100)}%`;

    this.renderSelectedFile();
    this.renderPreview();
    this.renderControls();
    this.placementEditor.render();
  }

  renderControls() {
    const dirty = this.isDirty();
    const busy = Boolean(this.state.upload) || this.state.pendingSave;
    const canEdit = this.state.received || this.state.demo;
    this.elements.dirtyBadge.classList.toggle("mini-badge-hidden", !dirty);
    this.elements.resetButton.disabled = !dirty || busy;
    this.elements.saveButton.disabled = !dirty || busy || !canEdit || (!this.state.connected && !this.state.demo);
    this.elements.panelSelect.disabled = !canEdit || busy;
    this.elements.newPanelButton.disabled = !canEdit || busy || this.state.collectionDraft.panels.length >= 64;
    this.elements.panelIdInput.disabled = !canEdit || busy;
    this.elements.defaultPanelInput.disabled = !canEdit || busy;
    this.elements.duplicatePanelButton.disabled = !canEdit || busy || this.state.collectionDraft.panels.length >= 64;
    this.elements.deletePanelButton.disabled = !canEdit || busy || this.state.collectionDraft.panels.length <= 1;
    this.elements.titleInput.disabled = !canEdit || busy;
    this.elements.fitSelect.disabled = !canEdit || busy;
    this.elements.scaleInput.disabled = !canEdit || busy;
    this.elements.markerScaleInput.disabled = !canEdit || busy;

    const hasFile = Boolean(this.state.selectedFile);
    this.elements.fileInput.disabled = busy || !canEdit;
    this.elements.clearSelectionButton.disabled = !hasFile || busy;
    this.elements.uploadButton.disabled = !hasFile || busy || dirty || (!this.state.connected && !this.state.demo);
    this.elements.cancelUploadButton.classList.toggle("button-hidden", !this.state.upload);
    this.elements.dropZone.classList.toggle("upload-drop-zone-disabled", busy || !canEdit);
    this.placementEditor.setEnabled(canEdit && !busy);
  }

  renderSelectedFile() {
    const file = this.state.selectedFile;
    if (!file) {
      this.elements.selectedInfo.className = "selected-file selected-file-hidden";
      this.elements.selectedInfo.replaceChildren();
      return;
    }
    const dimensions = this.state.selectedDimensions;
    const meta = dimensions ? `${dimensions.width}×${dimensions.height} · ${formatBytes(file.size)}` : formatBytes(file.size);
    this.elements.selectedInfo.className = "selected-file";
    this.elements.selectedInfo.innerHTML = `
      <span class="selected-file-icon" aria-hidden="true">▧</span>
      <span><strong></strong><small></small></span>
    `;
    this.elements.selectedInfo.querySelector("strong").textContent = file.name;
    this.elements.selectedInfo.querySelector("small").textContent = meta;
  }

  renderPreview() {
    const background = this.state.draft.background;
    const selectedUrl = this.state.selectedPreviewUrl;
    const configuredUrl = background.file ? dashboardAssetUrl(background.file) : "";
    const url = selectedUrl || configuredUrl;
    const scale = this.state.previewScale || background.defaultScale;
    const percent = Math.round(scale * 100);
    this.elements.scaleValue.textContent = `${Math.round(background.defaultScale * 100)}%`;
    this.elements.previewMode.textContent = `${fitLabel(background.fit)} · просмотр ${percent}%`;
    this.elements.previewStage.dataset.fit = background.fit;
    this.elements.previewStage.style.setProperty("--preview-scale", String(scale));

    if (!url) {
      this.elements.previewImage.removeAttribute("src");
      this.elements.previewImage.classList.add("dashboard-preview-image-hidden");
      this.elements.previewPlaceholder.classList.remove("dashboard-preview-placeholder-hidden");
      document.getElementById("dashboardImageCanvas").classList.add("dashboard-image-canvas-empty");
      this.elements.backgroundMeta.textContent = "Изображение пока не загружено";
      this.elements.backgroundSettingsMeta.textContent = "Не загружено";
      return;
    }

    if (this.elements.previewImage.src !== new URL(url, window.location.href).href) {
      this.elements.previewImage.src = url;
    }
    this.elements.previewImage.classList.remove("dashboard-preview-image-hidden");
    this.elements.previewPlaceholder.classList.add("dashboard-preview-placeholder-hidden");
    document.getElementById("dashboardImageCanvas").classList.remove("dashboard-image-canvas-empty");
    const dimensions = this.state.selectedDimensions || {
      width: background.naturalWidth,
      height: background.naturalHeight,
    };
    const source = selectedUrl ? "Предварительный файл" : background.file;
    const backgroundText = dimensions.width && dimensions.height
      ? `${source} · ${dimensions.width}×${dimensions.height}`
      : source;
    this.elements.backgroundMeta.textContent = backgroundText;
    this.elements.backgroundSettingsMeta.textContent = backgroundText;
  }
}
