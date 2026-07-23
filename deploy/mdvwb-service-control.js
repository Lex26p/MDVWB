(function () {
    var DEVICE_ID = "MDVWB-Service-1";
    var SERVICE_NAME = "mdvwb.service";
    var commandRunning = false;

    defineVirtualDevice(DEVICE_ID, {
        title: "Управление сервисом MDVWB 1",
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
            ServiceStatus: {
                title: "Статус сервиса",
                type: "text",
                value: "Статус ещё не запрошен",
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

    function compactOutput(value) {
        var text = value || "";
        text = String(text).replace(/\r/g, "");
        text = text.replace(/^\s+|\s+$/g, "");
        text = text.replace(/\n+/g, " | ");
        if (text.length > 500) {
            text = text.substring(0, 497) + "...";
        }
        return text;
    }

    function formatStatus(rawStatus) {
        var status = compactOutput(rawStatus);

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

    function queryStatus(resultPrefix) {
        runShellCommand("systemctl is-active " + SERVICE_NAME + " 2>&1 || true", {
            captureOutput: true,
            captureErrorOutput: true,
            exitCallback: function (exitCode, capturedOutput, capturedErrorOutput) {
                var rawStatus = compactOutput(capturedOutput);
                if (!rawStatus) {
                    rawStatus = compactOutput(capturedErrorOutput);
                }

                var formattedStatus = formatStatus(rawStatus);
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
                var details = compactOutput(capturedOutput);
                var errors = compactOutput(capturedErrorOutput);
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

    setTimeout(function () {
        refreshStatus();
    }, 1500);
})();
