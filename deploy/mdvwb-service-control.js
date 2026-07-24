(function () {
    var DEVICE_ID = "MDVWB-Service-1";
    var SERVICE_NAME = "mdvwb.service";
    var DRIVER_PATH = "/usr/local/bin/MDVWB";
    var CONFIG_PATH = "/etc/default/mdvwb";
    var commandRunning = false;

    defineVirtualDevice(DEVICE_ID, {
        title: "Управление и диагностика MDVWB 1",
        cells: {
            Start: {
                title: "Запустить",
                type: "pushbutton",
                value: false,
                forceDefault: true
            },
            Stop: {
                title: "Остановить",
                type: "pushbutton",
                value: false,
                forceDefault: true
            },
            Restart: {
                title: "Перезапустить",
                type: "pushbutton",
                value: false,
                forceDefault: true
            },
            RefreshStatus: {
                title: "Обновить статус",
                type: "pushbutton",
                value: false,
                forceDefault: true
            },
            RefreshDiagnostics: {
                title: "Обновить диагностику",
                type: "pushbutton",
                value: false,
                forceDefault: true
            },
            DiscoverDevices: {
                title: "Найти устройства",
                type: "pushbutton",
                value: false,
                forceDefault: true
            },
            ServiceStatus: {
                title: "Статус сервиса",
                type: "text",
                value: "Статус ещё не запрошен",
                readonly: true,
                forceDefault: true
            },
            AutostartStatus: {
                title: "Автозапуск",
                type: "text",
                value: "Не проверен",
                readonly: true,
                forceDefault: true
            },
            DriverVersion: {
                title: "Версия драйвера",
                type: "text",
                value: "Не проверена",
                readonly: true,
                forceDefault: true
            },
            RecentJournal: {
                title: "Последние записи журнала",
                type: "text",
                value: "Журнал ещё не запрошен",
                readonly: true,
                forceDefault: true
            },
            DiscoveryDetails: {
                title: "Результат поиска",
                type: "text",
                value: "Результат отсутствует",
                readonly: true,
                forceDefault: true
            },
            Result: {
                title: "Результат команды",
                type: "text",
                value: "Готово",
                readonly: true,
                forceDefault: true
            }
        }
    });

    function setText(controlName, value) {
        dev[DEVICE_ID + "/" + controlName] = value;
    }

    function trimText(value) {
        return String(value || "").replace(/\r/g, "").replace(/^\s+|\s+$/g, "");
    }

    function compactOutput(value, maxLength) {
        var text = trimText(value).replace(/\n+/g, " | ");
        var limit = maxLength || 500;

        if (text.length > limit) {
            text = text.substring(0, limit - 3) + "...";
        }
        return text;
    }

    function formatStatus(rawStatus) {
        var status = compactOutput(rawStatus, 100);

        if (status === "active") {
            return "active — сервис работает";
        }
        if (status === "inactive") {
            return "inactive — сервис остановлен";
        }
        if (status === "failed") {
            return "failed — сервис завершился с ошибкой";
        }
        if (status === "activating") {
            return "activating — сервис запускается";
        }
        if (status === "deactivating") {
            return "deactivating — сервис останавливается";
        }
        if (!status || status === "unknown") {
            return "unknown — состояние не определено";
        }

        return status;
    }

    function formatAutostart(rawStatus) {
        var status = compactOutput(rawStatus, 100);

        if (status === "enabled") {
            return "enabled — включён";
        }
        if (status === "disabled") {
            return "disabled — выключен";
        }
        if (status === "static") {
            return "static — управляется зависимостями systemd";
        }
        if (status === "masked") {
            return "masked — запуск заблокирован";
        }
        if (!status) {
            return "unknown — состояние не определено";
        }

        return status;
    }

    function getSection(output, marker, nextMarker) {
        var startToken = marker + "\n";
        var start = output.indexOf(startToken);
        var end;

        if (start < 0) {
            return "";
        }

        start += startToken.length;
        end = nextMarker ? output.indexOf(nextMarker + "\n", start) : output.length;
        if (end < 0) {
            end = output.length;
        }

        return trimText(output.substring(start, end));
    }

    function queryStatus(resultPrefix) {
        runShellCommand("systemctl is-active " + SERVICE_NAME + " 2>&1 || true", {
            captureOutput: true,
            captureErrorOutput: true,
            exitCallback: function (exitCode, capturedOutput, capturedErrorOutput) {
                var rawStatus = compactOutput(capturedOutput, 100);
                var formattedStatus;

                if (!rawStatus) {
                    rawStatus = compactOutput(capturedErrorOutput, 100);
                }

                formattedStatus = formatStatus(rawStatus);
                setText("ServiceStatus", formattedStatus);

                if (resultPrefix) {
                    setText("Result", resultPrefix + "; " + formattedStatus);
                }

                commandRunning = false;
            }
        });
    }

    function executeServiceCommand(actionTitle, shellCommand) {
        if (commandRunning) {
            setText("Result", "Другая команда уже выполняется");
            return;
        }

        commandRunning = true;
        setText("Result", actionTitle + ": выполняется...");

        runShellCommand(shellCommand, {
            captureOutput: true,
            captureErrorOutput: true,
            exitCallback: function (exitCode, capturedOutput, capturedErrorOutput) {
                var details = compactOutput(capturedOutput, 300);
                var errors = compactOutput(capturedErrorOutput, 300);
                var result;

                if (exitCode === 0) {
                    result = actionTitle + ": успешно";
                } else {
                    result = actionTitle + ": ошибка, код " + exitCode;
                }

                if (details) {
                    result += " | " + details;
                }
                if (errors) {
                    result += " | " + errors;
                }

                queryStatus(result);
            }
        });
    }

    function refreshStatus() {
        if (commandRunning) {
            setText("Result", "Другая команда уже выполняется");
            return;
        }

        commandRunning = true;
        setText("Result", "Запрос статуса выполняется...");
        queryStatus("Статус обновлён");
    }

    function extractFoundAddresses(output) {
        var match = String(output || "").match(/(?:^|\n)FOUND_ADDRESSES=([0-9,]*)(?:\r?\n|$)/);

        if (!match) {
            return null;
        }
        return match[1];
    }


    function discoverDevices() {
        var command;

        if (commandRunning) {
            setText("Result", "Другая команда уже выполняется");
            return;
        }

        commandRunning = true;
        setText("DiscoveryDetails", "Поиск выполняется...");
        setText("Result", "Поиск устройств выполняется...");

        command =
            "service_before=$(systemctl is-active " + SERVICE_NAME + " 2>&1 || true); " +
            "printf '__SERVICE_BEFORE__\\n%s\\n' \"$service_before\"; " +
            "if [ \"$service_before\" = 'active' ] || [ \"$service_before\" = 'activating' ]; then " +
                "systemctl stop " + SERVICE_NAME + " || exit 21; " +
                "attempt=0; " +
                "while systemctl is-active --quiet " + SERVICE_NAME + " && [ \"$attempt\" -lt 25 ]; do " +
                    "sleep 0.2; attempt=$((attempt + 1)); " +
                "done; " +
                "if systemctl is-active --quiet " + SERVICE_NAME + "; then exit 22; fi; " +
            "fi; " +
            "if [ ! -r " + CONFIG_PATH + " ]; then echo 'Не найден файл " + CONFIG_PATH + "' >&2; exit 23; fi; " +
            ". " + CONFIG_PATH + "; " +
            "port=${MDVWB_PORT:-/dev/ttyRS485-1}; " +
            "master_id=${MDVWB_MASTER_ID:-0}; " +
            "period_ms=${MDVWB_PERIOD_MS:-150}; " +
            "timeout_ms=${MDVWB_RESPONSE_TIMEOUT_MS:-130}; " +
            "printf '__DISCOVERY_OUTPUT__\\n'; " +
            DRIVER_PATH + " --discover --port \"$port\" --master-id \"$master_id\" " +
                "--period-ms \"$period_ms\" --response-timeout-ms \"$timeout_ms\"; " +
            "discovery_code=$?; " +
            "printf '__DISCOVERY_EXIT__\\n%s\\n' \"$discovery_code\"; " +
            "printf '__SERVICE_AFTER__\\n'; " +
            "systemctl is-active " + SERVICE_NAME + " 2>&1 || true; " +
            "exit \"$discovery_code\"";

        runShellCommand(command, {
            captureOutput: true,
            captureErrorOutput: true,
            exitCallback: function (exitCode, capturedOutput, capturedErrorOutput) {
                var output = trimText(capturedOutput);
                var discoveryOutput = getSection(output, "__DISCOVERY_OUTPUT__", "__DISCOVERY_EXIT__");
                var serviceAfter = getSection(output, "__SERVICE_AFTER__", "");
                var addresses = extractFoundAddresses(discoveryOutput);
                var displayAddresses;
                var result;

                setText("ServiceStatus", formatStatus(serviceAfter));

                if (exitCode === 0 && addresses !== null) {
                    displayAddresses = addresses ? addresses.replace(/,/g, ", ") : "нет устройств";
                    result = "На связи: " + displayAddresses;
                    setText("DiscoveryDetails", result);
                    setText("Result", result);
                } else {
                    result = "Ошибка поиска: код " + exitCode;
                    setText("DiscoveryDetails", result);
                    setText("Result", result);
                }
                commandRunning = false;
            }
        });
    }

    function refreshDiagnostics() {
        var command;

        if (commandRunning) {
            setText("Result", "Другая команда уже выполняется");
            return;
        }

        commandRunning = true;
        setText("Result", "Диагностика обновляется...");

        command = "printf '__STATUS__\\n'; " +
            "systemctl is-active " + SERVICE_NAME + " 2>&1 || true; " +
            "printf '__AUTOSTART__\\n'; " +
            "systemctl is-enabled " + SERVICE_NAME + " 2>&1 || true; " +
            "printf '__VERSION__\\n'; " +
            DRIVER_PATH + " --version 2>&1 || true; " +
            "printf '__JOURNAL__\\n'; " +
            "journalctl -u " + SERVICE_NAME + " -n 12 --no-pager -o cat 2>&1 || true";

        runShellCommand(command, {
            captureOutput: true,
            captureErrorOutput: true,
            exitCallback: function (exitCode, capturedOutput, capturedErrorOutput) {
                var output = trimText(capturedOutput);
                var errors = compactOutput(capturedErrorOutput, 300);
                var rawStatus = getSection(output, "__STATUS__", "__AUTOSTART__");
                var rawAutostart = getSection(output, "__AUTOSTART__", "__VERSION__");
                var rawVersion = getSection(output, "__VERSION__", "__JOURNAL__");
                var rawJournal = getSection(output, "__JOURNAL__", "");

                setText("ServiceStatus", formatStatus(rawStatus));
                setText("AutostartStatus", formatAutostart(rawAutostart));
                setText("DriverVersion", compactOutput(rawVersion, 200) || "Версия не определена");
                setText("RecentJournal", compactOutput(rawJournal, 1200) || "Журнал пуст");

                if (errors) {
                    setText("Result", "Диагностика обновлена с сообщением: " + errors);
                } else {
                    setText("Result", "Диагностика обновлена");
                }

                commandRunning = false;
            }
        });
    }

    defineRule("mdvwb_service_start", {
        whenChanged: DEVICE_ID + "/Start",
        then: function () {
            executeServiceCommand("Запуск сервиса", "systemctl start " + SERVICE_NAME);
        }
    });

    defineRule("mdvwb_service_stop", {
        whenChanged: DEVICE_ID + "/Stop",
        then: function () {
            executeServiceCommand("Остановка сервиса", "systemctl stop " + SERVICE_NAME);
        }
    });

    defineRule("mdvwb_service_restart", {
        whenChanged: DEVICE_ID + "/Restart",
        then: function () {
            executeServiceCommand("Перезапуск сервиса", "systemctl restart " + SERVICE_NAME);
        }
    });

    defineRule("mdvwb_service_refresh_status", {
        whenChanged: DEVICE_ID + "/RefreshStatus",
        then: function () {
            refreshStatus();
        }
    });

    defineRule("mdvwb_service_refresh_diagnostics", {
        whenChanged: DEVICE_ID + "/RefreshDiagnostics",
        then: function () {
            refreshDiagnostics();
        }
    });

    defineRule("mdvwb_device_discovery", {
        whenChanged: DEVICE_ID + "/DiscoverDevices",
        then: function () {
            discoverDevices();
        }
    });

    setTimeout(function () {
        refreshDiagnostics();
    }, 1500);
})();
