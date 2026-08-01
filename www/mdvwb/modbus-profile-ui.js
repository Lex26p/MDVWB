import {
  findModbusProfile,
  normalizeModbusProfileCatalog,
} from "./model.js";

const CAPABILITY_LABELS = new Map([
  ["power", "Power"],
  ["mode", "Mode"],
  ["fanSpeed", "FanSpeed"],
  ["setTemperature", "SetTemperature"],
  ["roomTemperature", "RoomTemperature"],
  ["alarm", "Alarm"],
  ["blinds", "Blinds"],
  ["blocked", "Blocked"],
]);

function createField(labelText, element) {
  const label = document.createElement("label");
  label.className = "field";
  const caption = document.createElement("span");
  caption.textContent = labelText;
  label.append(caption, element);
  return label;
}

function createReadonlyInput(id) {
  const input = document.createElement("input");
  input.id = id;
  input.type = "text";
  input.readOnly = true;
  input.setAttribute("aria-readonly", "true");
  return input;
}

function accessLabel(capability) {
  if (capability.readable && capability.writable) {
    return "чтение и запись";
  }
  if (capability.readable) {
    return "только чтение";
  }
  if (capability.writable) {
    return "только запись";
  }
  return "без доступа";
}

export function describeModbusCapabilities(profile) {
  if (!profile) {
    return "Возможности профиля недоступны.";
  }

  const values = Object.entries(profile.capabilities)
    .filter(([, capability]) => capability.supported)
    .map(([name, capability]) =>
      `${CAPABILITY_LABELS.get(name) || name}: ${accessLabel(capability)}`);

  return values.length > 0
    ? values.join(" · ")
    : "Профиль не объявляет пользовательских возможностей.";
}

export function protocolDisplayName(bus, profileCatalog) {
  if (bus.protocol !== "modbus_rtu") {
    return "MDV";
  }
  const profile = findModbusProfile(
    profileCatalog,
    bus.modbus?.profileId || "",
  );
  return profile
    ? `Modbus RTU · ${profile.name}`
    : `Modbus RTU · ${bus.modbus?.profileId || "профиль не указан"}`;
}

export class ModbusBusEditor {
  constructor({ form, addressInput }) {
    if (!form || !addressInput) {
      throw new Error("Редактор Modbus не может найти форму шины");
    }

    this.catalog = normalizeModbusProfileCatalog({
      schemaVersion: 1,
      profiles: [],
      issues: [],
    });
    this.fallbackSettings = null;
    this.changeHandler = null;

    const grid = form.querySelector(".form-grid");
    const addressField = addressInput.closest(".field");
    if (!grid || !addressField) {
      throw new Error("Редактор Modbus не может встроить поля протокола");
    }

    this.protocolInput = document.createElement("select");
    this.protocolInput.id = "busProtocolInput";
    this.protocolInput.innerHTML =
      '<option value="mdv">MDV</option>' +
      '<option value="modbus_rtu">Modbus RTU</option>';
    const protocolField = createField("Протокол", this.protocolInput);

    this.profileInput = document.createElement("select");
    this.profileInput.id = "busModbusProfileInput";
    this.profileField = createField("Профиль Modbus", this.profileInput);

    this.transportPanel = document.createElement("section");
    this.transportPanel.className = "field field-wide";
    this.transportPanel.setAttribute("aria-label", "Параметры Modbus RTU");

    const transportTitle = document.createElement("span");
    transportTitle.textContent = "Параметры линии из профиля";

    const transportGrid = document.createElement("div");
    transportGrid.className = "form-grid";

    this.baudRateInput = createReadonlyInput("busModbusBaudRate");
    this.dataBitsInput = createReadonlyInput("busModbusDataBits");
    this.parityInput = createReadonlyInput("busModbusParity");
    this.stopBitsInput = createReadonlyInput("busModbusStopBits");

    transportGrid.append(
      createField("Скорость, бод", this.baudRateInput),
      createField("Биты данных", this.dataBitsInput),
      createField("Чётность", this.parityInput),
      createField("Стоп-биты", this.stopBitsInput),
    );

    const transportHint = document.createElement("small");
    transportHint.textContent =
      "Значения задаются выбранным профилем и проверяются менеджером перед запуском.";

    this.transportPanel.append(transportTitle, transportGrid, transportHint);

    this.capabilitySummary = document.createElement("p");
    this.capabilitySummary.className = "muted field-wide";
    this.capabilitySummary.id = "busModbusCapabilitySummary";

    this.catalogStatus = document.createElement("p");
    this.catalogStatus.className = "muted field-wide";
    this.catalogStatus.id = "busModbusCatalogStatus";

    grid.insertBefore(protocolField, addressField);
    grid.insertBefore(this.profileField, addressField);
    grid.insertBefore(this.transportPanel, addressField);
    grid.insertBefore(this.capabilitySummary, addressField);
    grid.insertBefore(this.catalogStatus, addressField);

    this.addressHint = addressInput.parentElement?.querySelector("small") || null;

    this.protocolInput.addEventListener("change", () => {
      this.render();
      this.notifyChange();
    });
    this.profileInput.addEventListener("change", () => {
      this.fallbackSettings = null;
      this.render();
      this.notifyChange();
    });

    this.render();
  }

  setChangeHandler(handler) {
    this.changeHandler = typeof handler === "function" ? handler : null;
  }

  notifyChange() {
    if (this.changeHandler) {
      this.changeHandler();
    }
  }

  setCatalog(value) {
    const selected = this.profileInput.value;
    this.catalog = normalizeModbusProfileCatalog(value);
    this.populateProfiles(selected);
    this.render();
  }

  open(bus = null) {
    this.protocolInput.value = bus?.protocol === "modbus_rtu"
      ? "modbus_rtu"
      : "mdv";
    this.fallbackSettings = bus?.modbus ? { ...bus.modbus } : null;
    this.populateProfiles(bus?.modbus?.profileId || "");
    this.render();
  }

  reset() {
    this.protocolInput.value = "mdv";
    this.fallbackSettings = null;
    this.populateProfiles("");
    this.render();
  }

  values() {
    return {
      protocol: this.protocolInput.value,
      profileId: this.protocolInput.value === "modbus_rtu"
        ? this.profileInput.value
        : "",
    };
  }

  currentProfile() {
    return findModbusProfile(this.catalog, this.profileInput.value);
  }

  populateProfiles(preferredId) {
    const previous = preferredId || this.profileInput.value;
    this.profileInput.replaceChildren();

    for (const profile of this.catalog.profiles) {
      const option = document.createElement("option");
      option.value = profile.id;
      option.textContent = profile.name;
      this.profileInput.append(option);
    }

    if (previous &&
        !this.catalog.profiles.some((profile) => profile.id === previous)) {
      const option = document.createElement("option");
      option.value = previous;
      option.textContent = `${previous} (недоступен)`;
      option.dataset.unavailable = "true";
      this.profileInput.append(option);
    }

    if (previous && [...this.profileInput.options].some((option) => option.value === previous)) {
      this.profileInput.value = previous;
    } else if (this.profileInput.options.length > 0) {
      this.profileInput.selectedIndex = 0;
    }
  }

  render() {
    const modbus = this.protocolInput.value === "modbus_rtu";
    this.profileField.hidden = !modbus;
    this.transportPanel.hidden = !modbus;
    this.capabilitySummary.hidden = !modbus;
    this.catalogStatus.hidden = !modbus;

    if (!modbus) {
      if (this.addressHint) {
        this.addressHint.textContent = "Адреса 0–63 через запятую.";
      }
      return;
    }

    const profile = this.currentProfile();
    const settings = profile?.transport || this.fallbackSettings || null;

    this.baudRateInput.value = settings ? String(settings.baudRate) : "—";
    this.dataBitsInput.value = settings ? String(settings.dataBits) : "—";
    this.parityInput.value = settings ? String(settings.parity) : "—";
    this.stopBitsInput.value = settings ? String(settings.stopBits) : "—";

    this.capabilitySummary.textContent = describeModbusCapabilities(profile);

    const issueText = this.catalog.issues
      .map((issue) => `${issue.file}: ${issue.message}`)
      .join(" · ");
    if (profile) {
      this.catalogStatus.textContent = issueText
        ? `Некоторые профили не загружены: ${issueText}`
        : "Профиль получен от менеджера.";
    } else if (this.catalog.profiles.length === 0) {
      this.catalogStatus.textContent = issueText
        ? `Каталог профилей недоступен: ${issueText}`
        : "Каталог профилей ещё не получен.";
    } else {
      this.catalogStatus.textContent =
        "Выбранный профиль отсутствует в каталоге. Сохранение Modbus-шины запрещено.";
    }

    if (this.addressHint) {
      const range = profile?.logicalAddresses;
      this.addressHint.textContent = range
        ? `Логические адреса ${range.minimum}–${range.maximum} через запятую.`
        : "Логические адреса Modbus 1–63 через запятую.";
    }
  }
}
