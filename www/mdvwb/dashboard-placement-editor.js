import {
  availableDashboardDevices,
  cloneDashboardConfiguration,
  createFanPlacement,
  deviceKey,
  inspectDashboardPlacement,
} from "./dashboard-model.js";

function percent(value) {
  return Math.round(Number(value) * 100);
}

function bounded(value, minimum, maximum) {
  return Math.max(minimum, Math.min(maximum, Number(value) || 0));
}

function snapCoordinate(value) {
  return Math.round(bounded(value, 0, 1) * 100) / 100;
}

function copyFans(dashboard) {
  return cloneDashboardConfiguration(dashboard).fans;
}

function fanTitle(fan) {
  return `Фанкойл №${fan.number} · ${fan.label} · Fan-${fan.bus}_${fan.address}`;
}

function initialPosition(index) {
  const column = index % 10;
  const row = Math.floor(index / 10) % 10;
  return {
    x: snapCoordinate(0.1 + column * 0.08),
    y: snapCoordinate(0.1 + row * 0.08),
  };
}

export class DashboardPlacementEditor {
  constructor({ getDashboard, getMarkerScale, onFansChanged }) {
    this.getDashboard = getDashboard;
    this.getMarkerScale = typeof getMarkerScale === "function" ? getMarkerScale : () => 1;
    this.onFansChanged = onFansChanged;
    this.busConfiguration = { version: 1, buses: [] };
    this.selectedId = null;
    this.enabled = false;
    this.drag = null;
    this.gridEnabled = true;

    this.elements = {
      availableCount: document.getElementById("dashboardAvailableDeviceCount"),
      placedCount: document.getElementById("dashboardPlacedDeviceCount"),
      deviceList: document.getElementById("dashboardDeviceList"),
      emptyDevices: document.getElementById("dashboardDeviceEmpty"),
      markerLayer: document.getElementById("dashboardMarkerLayer"),
      imageCanvas: document.getElementById("dashboardImageCanvas"),
      gridInput: document.getElementById("dashboardGridInput"),
      inspectorEmpty: document.getElementById("dashboardInspectorEmpty"),
      inspectorForm: document.getElementById("dashboardInspectorForm"),
      inspectorReference: document.getElementById("dashboardInspectorReference"),
      numberInput: document.getElementById("dashboardMarkerNumber"),
      labelInput: document.getElementById("dashboardMarkerLabel"),
      xInput: document.getElementById("dashboardMarkerX"),
      yInput: document.getElementById("dashboardMarkerY"),
      centerButton: document.getElementById("dashboardMarkerCenter"),
      removeButton: document.getElementById("dashboardMarkerRemove"),
      issue: document.getElementById("dashboardMarkerIssue"),
    };

    this.bindEvents();
    this.renderGrid();
  }

  setBusConfiguration(configuration) {
    this.busConfiguration = configuration && Array.isArray(configuration.buses)
      ? configuration
      : { version: 1, buses: [] };
    this.render();
  }

  setEnabled(enabled) {
    const next = Boolean(enabled);
    if (this.enabled === next) {
      return;
    }
    this.enabled = next;
    this.render();
  }

  clearSelection() {
    this.selectedId = null;
    this.drag = null;
    this.render();
  }

  clearSelectionIfMissing() {
    const dashboard = this.getDashboard();
    if (this.selectedId && !dashboard.fans.some((fan) => fan.id === this.selectedId)) {
      this.selectedId = null;
    }
  }

  bindEvents() {
    this.elements.deviceList.addEventListener("change", (event) => {
      const deviceCheckbox = event.target.closest("input[data-device-key]");
      if (deviceCheckbox) {
        const [bus, address] = deviceCheckbox.dataset.deviceKey.split(":").map(Number);
        try {
          this.setDeviceVisible(bus, address, deviceCheckbox.checked);
        } catch (error) {
          window.alert(error.message);
          this.render();
        }
        return;
      }

      const busCheckbox = event.target.closest("input[data-bus-id]");
      if (busCheckbox) {
        try {
          this.setBusVisible(Number(busCheckbox.dataset.busId), busCheckbox.checked);
        } catch (error) {
          window.alert(error.message);
          this.render();
        }
      }
    });

    this.elements.deviceList.addEventListener("click", (event) => {
      const button = event.target.closest("button[data-device-key]");
      if (!button || button.disabled) {
        return;
      }
      const [bus, address] = button.dataset.deviceKey.split(":").map(Number);
      const placement = this.getDashboard().fans.find((fan) => fan.bus === bus && fan.address === address);
      if (!placement) {
        return;
      }
      this.selectedId = placement.id;
      this.render();
      this.focusSelectedMarker();
    });

    this.elements.markerLayer.addEventListener("click", (event) => {
      const marker = event.target.closest("button[data-placement-id]");
      if (!marker) {
        return;
      }
      this.selectedId = marker.dataset.placementId;
      this.render();
    });

    this.elements.markerLayer.addEventListener("pointerdown", (event) => {
      const marker = event.target.closest("button[data-placement-id]");
      if (!marker || !this.enabled || event.button !== 0) {
        return;
      }
      const fan = this.getDashboard().fans.find((item) => item.id === marker.dataset.placementId);
      const rectangle = this.elements.imageCanvas.getBoundingClientRect();
      if (!fan || rectangle.width <= 0 || rectangle.height <= 0) {
        return;
      }
      event.preventDefault();
      this.selectedId = fan.id;
      this.drag = {
        pointerId: event.pointerId,
        marker,
        startClientX: event.clientX,
        startClientY: event.clientY,
        startX: fan.x,
        startY: fan.y,
        canvasWidth: rectangle.width,
        canvasHeight: rectangle.height,
        moved: false,
      };
      marker.setPointerCapture(event.pointerId);
      marker.classList.add("fan-marker-dragging", "fan-marker-selected");
      this.renderDeviceList();
      this.renderInspector();
    });

    this.elements.markerLayer.addEventListener("pointermove", (event) => {
      if (!this.drag || event.pointerId !== this.drag.pointerId) {
        return;
      }
      const deltaX = event.clientX - this.drag.startClientX;
      const deltaY = event.clientY - this.drag.startClientY;
      if (!this.drag.moved && Math.hypot(deltaX, deltaY) < 4) {
        return;
      }
      event.preventDefault();
      this.drag.moved = true;
      const x = snapCoordinate(this.drag.startX + deltaX / this.drag.canvasWidth);
      const y = snapCoordinate(this.drag.startY + deltaY / this.drag.canvasHeight);
      this.updateSelectedDuringDrag(x, y);
    });

    const finishDrag = (event) => {
      if (!this.drag || event.pointerId !== this.drag.pointerId) {
        return;
      }
      const { marker } = this.drag;
      marker.classList.remove("fan-marker-dragging");
      if (marker.hasPointerCapture(event.pointerId)) {
        marker.releasePointerCapture(event.pointerId);
      }
      this.drag = null;
      this.render();
    };
    this.elements.markerLayer.addEventListener("pointerup", finishDrag);
    this.elements.markerLayer.addEventListener("pointercancel", finishDrag);

    this.elements.markerLayer.addEventListener("keydown", (event) => {
      const marker = event.target.closest("button[data-placement-id]");
      if (!marker || !this.enabled) {
        return;
      }
      const directions = {
        ArrowLeft: [-1, 0],
        ArrowRight: [1, 0],
        ArrowUp: [0, -1],
        ArrowDown: [0, 1],
      };
      const direction = directions[event.key];
      if (!direction) {
        return;
      }
      event.preventDefault();
      this.selectedId = marker.dataset.placementId;
      const step = event.shiftKey ? 0.05 : 0.01;
      this.updateSelected((fan) => ({
        ...fan,
        x: snapCoordinate(fan.x + direction[0] * step),
        y: snapCoordinate(fan.y + direction[1] * step),
      }));
      this.focusSelectedMarker();
    });

    this.elements.numberInput.addEventListener("change", () => {
      const number = Number(this.elements.numberInput.value);
      const dashboard = this.getDashboard();
      const duplicate = dashboard.fans.some((fan) => fan.id !== this.selectedId && Number(fan.number) === number);
      if (!Number.isInteger(number) || number < 1 || number > 200 || duplicate) {
        this.elements.numberInput.setCustomValidity(
          duplicate ? `Номер ${number} уже используется` : "Введите целое число от 1 до 200",
        );
        this.elements.numberInput.reportValidity();
        const selected = dashboard.fans.find((fan) => fan.id === this.selectedId);
        if (selected) {
          this.elements.numberInput.value = String(selected.number);
        }
        return;
      }
      this.elements.numberInput.setCustomValidity("");
      this.updateSelected((fan) => ({ ...fan, number }));
    });

    this.elements.labelInput.addEventListener("input", () => {
      this.updateSelected((fan) => ({ ...fan, label: this.elements.labelInput.value }), false);
    });
    this.elements.labelInput.addEventListener("change", () => this.render());

    this.elements.xInput.addEventListener("change", () => {
      this.updateSelected((fan) => ({
        ...fan,
        x: snapCoordinate(Number(this.elements.xInput.value) / 100),
      }));
    });
    this.elements.yInput.addEventListener("change", () => {
      this.updateSelected((fan) => ({
        ...fan,
        y: snapCoordinate(Number(this.elements.yInput.value) / 100),
      }));
    });
    this.elements.centerButton.addEventListener("click", () => {
      this.updateSelected((fan) => ({ ...fan, x: 0.5, y: 0.5 }));
      this.focusSelectedMarker();
    });
    this.elements.removeButton.addEventListener("click", () => this.removeSelected());

    this.elements.gridInput?.addEventListener("change", () => {
      this.gridEnabled = this.elements.gridInput.checked;
      this.renderGrid();
    });
  }

  renderGrid() {
    this.elements.imageCanvas.classList.toggle("dashboard-grid-enabled", this.gridEnabled);
    if (this.elements.gridInput) {
      this.elements.gridInput.checked = this.gridEnabled;
    }
  }

  setDeviceVisible(bus, address, visible) {
    if (!this.enabled) {
      return;
    }
    const dashboard = this.getDashboard();
    const fans = copyFans(dashboard);
    const index = fans.findIndex((fan) => fan.bus === bus && fan.address === address);

    if (index >= 0) {
      fans[index] = { ...fans[index], visible };
      this.selectedId = visible ? fans[index].id : (this.selectedId === fans[index].id ? null : this.selectedId);
    } else if (visible) {
      const position = initialPosition(fans.length);
      const placement = createFanPlacement({
        bus,
        address,
        label: `Fan-${bus}_${address}`,
        ...position,
      }, fans);
      placement.markerScale = bounded(this.getMarkerScale(), 0.5, 3);
      fans.push(placement);
      this.selectedId = placement.id;
    }

    this.onFansChanged(fans);
    this.render();
    if (visible) {
      this.focusSelectedMarker();
    }
  }

  setBusVisible(busId, visible) {
    if (!this.enabled) {
      return;
    }
    const devices = availableDashboardDevices(this.busConfiguration).filter((device) => device.bus === busId);
    const fans = copyFans(this.getDashboard());
    let lastAdded = null;

    devices.forEach((device) => {
      const index = fans.findIndex((fan) => fan.bus === device.bus && fan.address === device.address);
      if (index >= 0) {
        fans[index] = { ...fans[index], visible };
        if (!visible && this.selectedId === fans[index].id) {
          this.selectedId = null;
        }
        return;
      }
      if (!visible) {
        return;
      }
      const placement = createFanPlacement({
        bus: device.bus,
        address: device.address,
        label: device.name,
        ...initialPosition(fans.length),
      }, fans);
      placement.markerScale = bounded(this.getMarkerScale(), 0.5, 3);
      fans.push(placement);
      lastAdded = placement;
    });

    if (lastAdded) {
      this.selectedId = lastAdded.id;
    }
    this.onFansChanged(fans);
    this.render();
  }

  removeSelected() {
    if (!this.selectedId || !this.enabled) {
      return;
    }
    const dashboard = this.getDashboard();
    const fan = dashboard.fans.find((item) => item.id === this.selectedId);
    if (!fan) {
      return;
    }
    if (!window.confirm(`Полностью удалить «${fan.label}» из конфигурации панели?`)) {
      return;
    }
    const fans = copyFans(dashboard).filter((item) => item.id !== this.selectedId);
    this.selectedId = null;
    this.onFansChanged(fans);
    this.render();
  }

  updateSelected(mutator, render = true) {
    if (!this.selectedId || !this.enabled) {
      return;
    }
    const fans = copyFans(this.getDashboard());
    const index = fans.findIndex((fan) => fan.id === this.selectedId);
    if (index < 0) {
      return;
    }
    fans[index] = mutator(fans[index]);
    this.onFansChanged(fans);
    if (render) {
      this.render();
    } else {
      this.renderMarkers();
    }
  }

  updateSelectedDuringDrag(x, y) {
    if (!this.selectedId || !this.enabled || !this.drag) {
      return;
    }
    const fans = copyFans(this.getDashboard());
    const index = fans.findIndex((fan) => fan.id === this.selectedId);
    if (index < 0) {
      return;
    }
    fans[index] = { ...fans[index], x, y };
    this.onFansChanged(fans);
    this.drag.marker.style.left = `${x * 100}%`;
    this.drag.marker.style.top = `${y * 100}%`;
    this.elements.xInput.value = String(percent(x));
    this.elements.yInput.value = String(percent(y));
  }

  focusSelectedMarker() {
    window.requestAnimationFrame(() => {
      this.elements.markerLayer
        .querySelector(`button[data-placement-id="${CSS.escape(this.selectedId || "")}"]`)
        ?.focus({ preventScroll: true });
    });
  }

  render() {
    this.clearSelectionIfMissing();
    this.renderDeviceList();
    this.renderMarkers();
    this.renderInspector();
    this.renderGrid();
  }

  renderDeviceList() {
    const dashboard = this.getDashboard();
    const devices = availableDashboardDevices(this.busConfiguration);
    const placements = new Map(dashboard.fans.map((fan) => [deviceKey(fan.bus, fan.address), fan]));
    const visibleCount = dashboard.fans.filter((fan) => fan.visible).length;
    this.elements.availableCount.textContent = String(devices.length);
    this.elements.placedCount.textContent = String(visibleCount);
    this.elements.deviceList.replaceChildren();
    const configuredBuses = Array.isArray(this.busConfiguration.buses) ? this.busConfiguration.buses : [];
    this.elements.emptyDevices.classList.toggle("device-empty-hidden", configuredBuses.length > 0);

    const groups = new Map(configuredBuses
      .slice()
      .sort((left, right) => Number(left.id) - Number(right.id))
      .map((bus) => [Number(bus.id), []]));
    devices.forEach((device) => {
      if (!groups.has(device.bus)) {
        groups.set(device.bus, []);
      }
      groups.get(device.bus).push(device);
    });

    groups.forEach((busDevices, busId) => {
      const group = document.createElement("section");
      group.className = "device-bus-group";
      const bus = this.busConfiguration.buses.find((item) => item.id === busId);
      const visibleDevices = busDevices.filter((device) => placements.get(device.key)?.visible).length;

      const heading = document.createElement("label");
      heading.className = "device-bus-heading device-bus-toggle";
      const busCheckbox = document.createElement("input");
      busCheckbox.type = "checkbox";
      busCheckbox.dataset.busId = String(busId);
      busCheckbox.checked = busDevices.length > 0 && visibleDevices === busDevices.length;
      busCheckbox.indeterminate = visibleDevices > 0 && visibleDevices < busDevices.length;
      busCheckbox.disabled = !this.enabled || busDevices.length === 0;
      const headingText = document.createElement("span");
      headingText.innerHTML = `<strong>Шина ${busId}</strong><small></small>`;
      headingText.querySelector("small").textContent = `${visibleDevices}/${busDevices.length} на карте${bus?.enabled ? "" : " · отключена"}`;
      heading.append(busCheckbox, headingText);
      group.appendChild(heading);

      if (busDevices.length === 0) {
        const empty = document.createElement("p");
        empty.className = "device-bus-empty";
        empty.textContent = "На этой шине пока не настроены адреса.";
        group.appendChild(empty);
      }

      busDevices.forEach((device) => {
        const placement = placements.get(device.key);
        const row = document.createElement("div");
        row.dataset.placementId = placement?.id || "";
        row.className = [
          "device-catalog-item",
          placement?.visible ? "device-catalog-item-placed" : "",
          placement?.id === this.selectedId ? "device-catalog-item-selected" : "",
        ].filter(Boolean).join(" ");

        const checkbox = document.createElement("input");
        checkbox.type = "checkbox";
        checkbox.className = "device-map-checkbox";
        checkbox.dataset.deviceKey = device.key;
        checkbox.checked = placement?.visible === true;
        checkbox.disabled = !this.enabled;
        checkbox.setAttribute("aria-label", checkbox.checked ? `Убрать ${device.name} с карты` : `Добавить ${device.name} на карту`);

        const selectButton = document.createElement("button");
        selectButton.type = "button";
        selectButton.className = "device-catalog-select";
        selectButton.dataset.deviceKey = device.key;
        selectButton.disabled = !placement;
        selectButton.innerHTML = `<span class="device-mini-marker"></span><span class="device-catalog-text"><strong></strong><small></small></span>`;
        selectButton.querySelector(".device-mini-marker").textContent = placement ? String(placement.number) : "—";
        selectButton.querySelector("strong").textContent = placement ? `№${placement.number} · ${placement.label}` : device.name;
        selectButton.querySelector("small").textContent = `${device.name}${device.enabled ? "" : " · шина отключена"}`;

        row.append(checkbox, selectButton);
        group.appendChild(row);
      });
      this.elements.deviceList.appendChild(group);
    });
  }

  renderMarkers() {
    const dashboard = this.getDashboard();
    this.elements.markerLayer.replaceChildren();

    dashboard.fans.filter((fan) => fan.visible).forEach((fan) => {
      const issue = inspectDashboardPlacement(fan, this.busConfiguration);
      const marker = document.createElement("button");
      marker.type = "button";
      marker.className = [
        "fan-marker",
        "fan-marker-unknown",
        fan.id === this.selectedId ? "fan-marker-selected" : "",
        issue ? "fan-marker-reference-error" : "",
      ].filter(Boolean).join(" ");
      marker.dataset.placementId = fan.id;
      marker.style.left = `${fan.x * 100}%`;
      marker.style.top = `${fan.y * 100}%`;
      marker.style.setProperty("--marker-scale", String(fan.markerScale));
      marker.title = issue ? `${fanTitle(fan)} · ${issue.message}` : fanTitle(fan);
      marker.setAttribute("aria-label", marker.title);
      marker.innerHTML = `<span class="fan-marker-number"></span>`;
      marker.querySelector(".fan-marker-number").textContent = String(fan.number);
      this.elements.markerLayer.appendChild(marker);
    });
  }

  renderInspector() {
    const dashboard = this.getDashboard();
    const fan = dashboard.fans.find((item) => item.id === this.selectedId) || null;
    this.elements.inspectorEmpty.classList.toggle("marker-inspector-empty-hidden", Boolean(fan));
    this.elements.inspectorForm.classList.toggle("marker-inspector-form-hidden", !fan);
    if (!fan) {
      return;
    }

    const issue = inspectDashboardPlacement(fan, this.busConfiguration);
    this.elements.inspectorReference.textContent = `Fan-${fan.bus}_${fan.address}`;
    this.elements.numberInput.setCustomValidity("");
    this.elements.numberInput.value = String(fan.number);
    this.elements.labelInput.value = fan.label;
    this.elements.xInput.value = String(percent(fan.x));
    this.elements.yInput.value = String(percent(fan.y));
    this.elements.issue.classList.toggle("marker-issue-hidden", !issue);
    this.elements.issue.textContent = issue?.message || "";

    [
      this.elements.numberInput,
      this.elements.labelInput,
      this.elements.xInput,
      this.elements.yInput,
      this.elements.centerButton,
      this.elements.removeButton,
    ].forEach((element) => { element.disabled = !this.enabled; });
  }
}
