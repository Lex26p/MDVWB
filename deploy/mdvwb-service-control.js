(function () {
    var DEVICE_ID = "MDVWB-Service-1";
    var SERVICE_NAME = "mdvwb.service";
    var DRIVER_PATH = "/usr/local/bin/MDVWB";
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

    setTimeout(function () {
        refreshDiagnostics();
    }, 1500);
})();
