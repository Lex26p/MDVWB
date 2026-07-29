# Конфигурация и выполнение расписаний MDVWB

> Самостоятельный контракт `schedules.json`, MQTT API manager и runtime `mdvwb-scheduler` для MDVWB 1.2.0.

## 1. Runtime files

```text
/etc/mdvwb/schedules.json
/etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
/etc/default/mdvwb-scheduler
/var/lib/mdvwb/scheduler-state.tsv
/usr/local/bin/mdvwb-scheduler
/etc/systemd/system/mdvwb-scheduler.service
```

Manager — единственный writer. Scheduler только читает configuration.

## 2. Пустая конфигурация

```json
{
  "version": 1,
  "revision": 0,
  "schedules": []
}
```

Manager создаёт её atomically, только если file отсутствует.

## 3. Полный пример

```json
{
  "version": 1,
  "revision": 7,
  "schedules": [
    {
      "id": "workday-start",
      "name": "Начало рабочего дня",
      "enabled": true,
      "panelId": "main",
      "kind": "weekly",
      "days": [1, 2, 3, 4, 5],
      "date": "",
      "time": "08:00",
      "targets": [
        {"bus": 1, "address": 1}
      ],
      "actions": {
        "power": true,
        "mode": 0,
        "speed": 2,
        "setTemp": 23
      }
    }
  ]
}
```

## 4. Root schema

```text
version = 1
revision = 0..2147483647
schedules = 0..256
```

Unknown fields, duplicate keys и duplicate schedule IDs запрещены. Canonical order — по `id`.

## 5. Schedule fields

```text
id
name
enabled
panelId
kind
days
date
time
targets
actions
```

Все обязательны.

## 6. ID и name

```text
id = 1..64, [A-Za-z0-9_-]
name = 1..128 bytes
panelId = 1..64, [A-Za-z0-9_-]
```

ID глобально уникален.

## 7. Weekly

```text
kind = weekly
days = непустой unique subset 1..7
date = ""
time = valid HH:MM
```

`1` — Monday, `7` — Sunday.

## 8. Once

```text
kind = once
days = []
date = valid YYYY-MM-DD, year 2000..2099
time = valid HH:MM
```

Проверяются реальные даты и leap years.

## 9. Targets

```text
count = 1..512
bus = 1..999
address = 0..63
```

Duplicate pair запрещена. Canonical order — bus/address.

## 10. Actions

Минимум один:

```text
power = boolean
mode = 0..4
speed = 1..4
setTemp = 16..32
```

Неуказанный action не меняется.

## 11. Reference validation

Нужны:

- existing panel;
- existing bus;
- existing address;
- visible placement на этой panel.

Issue kinds:

```text
MissingPanel
MissingBus
MissingAddress
TargetNotInPanel
```

## 12. Disabled bus

Stored schedule может ссылаться на disabled bus. Runtime scheduler такую target отклоняет до commands.

Manual run disabled schedule разрешён; disabled target bus — нет.

## 13. Configuration topics

```text
/mdvwb/schedules/config
/mdvwb/schedules/config/set
/mdvwb/schedules/config/result
/mdvwb/schedules/status
```

Config/status retained. Set/result non-retained. Set limit — 1 MiB.

## 14. Optimistic save

```text
submitted.revision == current.revision
```

Success увеличивает revision.

Conflict order:

```text
result
current retained config
current status
```

File не изменяется.

## 15. Error result caveat

Только conflict гарантированно сообщает current counts. Другие ошибки текущей реализации могут вернуть нули.

Retained config остаётся source of truth.

## 16. Manual run topics

```text
/mdvwb/schedules/<id>/run
/mdvwb/schedules/<id>/execute
/mdvwb/schedules/<id>/result
```

Allowed `/run` payload:

```text
empty | 1 | run | {}
```

Manager валидирует и публикует internal `/execute`.

## 17. Manager и scheduler result

Manager может первым опубликовать preliminary `queued`.

Scheduler result отличается:

```json
"origin": "scheduler"
```

Именно scheduler result является authoritative execution result.

## 18. Service и executable

```text
mdvwb-scheduler.service
/usr/local/bin/mdvwb-scheduler
```

Unit использует `Restart=on-failure` и environment file `/etc/default/mdvwb-scheduler`.

## 19. Environment

```text
MDVWB_SCHEDULES_CONFIG=/etc/mdvwb/schedules.json
MDVWB_BUSES_CONFIG=/etc/mdvwb/buses.json
MDVWB_DASHBOARD_CONFIG=/etc/mdvwb/dashboard.json
MDVWB_SCHEDULER_STATE=/var/lib/mdvwb/scheduler-state.tsv
MDVWB_SCHEDULER_CONFIRM_TIMEOUT=10
```

Timeout range: `1..300`.

## 20. Subscriptions

```text
/mdvwb/schedules/config
/mdvwb/schedules/+/execute
/devices/+/controls/+
```

Config MQTT payload — notification. Source of truth — files on disk.

## 21. Freshness

Scheduler hashes full contents of:

```text
schedules.json
buses.json
dashboard.json
```

Это обнаруживает изменения независимо от size/mtime и MQTT notification.

## 22. Invalid reload

Invalid schedules-only update сохраняет last known good schedules в memory.

Invalid changed buses/dashboard dependency блокирует execution, очищает queue и завершает active run.

Valid repair unblocks автоматически.

## 23. Controller time

Automatic due использует localtime controller с точностью до minute.

Проверка:

```bash
date
timedatectl status
```

## 24. Due rules

Weekly: enabled + weekday + HH:MM.

Once: enabled + YYYY-MM-DD + HH:MM.

## 25. Missed once

Past once получает:

```text
state = missed
```

Late commands не отправляются. Marker сохраняется.

## 26. State file

```text
/var/lib/mdvwb/scheduler-state.tsv
```

Markers:

```text
schedule-id<TAB>YYYY-MM-DDTHH:MM
schedule-id<TAB>missed:YYYY-MM-DDTHH:MM
```

State предотвращает duplicate automatic run после restart в той же minute.

## 27. Queues

Incoming:

```text
1024 messages
2 MiB
key = full topic
```

Run queue:

```text
128 runs
256 KiB
key = scheduleId
```

Один active run.

## 28. Execution validation

Перед start:

- current revision;
- schedule exists;
- automatic enabled;
- references valid;
- buses enabled.

Stale queued request отклоняется.

## 29. Command order

```text
Mode
Speed
SetTemp
Power
```

Power last. Commands non-retained, индивидуальные.

## 30. No rollback

Несколько commands/targets не образуют transaction.

При failure/timeout часть изменений может быть выполнена. Rollback отсутствует.

## 31. Confirmation

Нужно:

```text
matching base topic
matching payload
after command sequence
retained = false
```

Retained replay не подтверждает run.

## 32. Offline

Fresh non-retained target `Status=7` завершает active run как failed.

Retained `Status=7` не считается новым offline event.

## 33. Timeout

Один deadline для всего run:

```text
Confirmation timeout: X/Y values confirmed
```

После timeout queue продолжает работу.

## 34. Result states

```text
queued
executing
completed
timeout
failed
rejected
missed
```

Scheduler result non-retained и содержит source, origin, controller time, commands и confirmed.

## 35. Scheduler status

```text
/mdvwb/scheduler/status
```

Retained payload содержит runtime state, revision, counts, queue, active schedule и controller clock.

## 36. Scheduler status и current browser integration

Backend status обновляется при событиях и каждую controller minute:

```text
/mdvwb/scheduler/status
```

Repository содержит helpers, которые различают retained/live delivery и используют threshold 125 seconds:

```text
www/fancoils/scheduler-status-ui.js
www/fancoils/scheduler-status-health.js
```

Но текущий `/fancoils/index.html` загружает только `app.js`, а `app.js` эти modules не импортирует.

Следовательно, production page сейчас не выполняет live-heartbeat freshness check и принимает retained scheduler status как обычное состояние.

## 37. Browser editor

Location:

```text
/fancoils/?panel=<id>
```

Поддерживает weekly/once, targets, four actions, duplicate/delete, optimistic save и manual run.

## 38. Current manual run UI

Run button требует:

- persisted clean draft;
- MQTT connection;
- отсутствие pending save;
- scheduler state `ready`, `executing` или `warning`.

Current `app.js` не требует fresh heartbeat, не фильтрует result по `origin="scheduler"` и не имеет 90-second client safety timeout.

Backend `origin`, controller time и terminal states остаются доступны внешним MQTT clients. Dormant status UI helper реализует более строгий flow, но пока не подключён к page.

## 39. Диагностика

```bash
systemctl status mdvwb-scheduler.service --no-pager
journalctl -u mdvwb-scheduler.service -n 150 --no-pager
cat /etc/default/mdvwb-scheduler
date
timedatectl status
```

## 40. MQTT диагностика

```bash
mosquitto_sub -v \
  -t '/mdvwb/schedules/#' \
  -t '/mdvwb/scheduler/status'
```

## 41. Safe manual test

```bash
mosquitto_sub -v -t '/mdvwb/schedules/workday-start/result'
```

В другом terminal:

```bash
mosquitto_pub -t '/mdvwb/schedules/workday-start/run' -m 'run'
```

Не используйте retained flag.

## 42. Профильные tests

```text
mdvwb_schedules_config_test
mdvwb_scheduler_test
mdvwb_scheduler_freshness_test
```

Они покрывают strict schema, references, timing, retained/live facts, timeout, state, missed once и dependency freshness.

## 43. Invariants

- Manager — one writer.
- Scheduler читает disk source of truth.
- Save использует revision.
- Targets должны быть visible.
- Runtime buses должны быть enabled.
- Automatic disabled schedule не запускается.
- Manual disabled schedule разрешён.
- Один active run.
- Broadcast отсутствует.
- Retained command/fact не используется как execution confirmation.
- Scheduler-level retry отсутствует.
- Rollback отсутствует.
- Missed once не выполняется поздно.
- Controller clock определяет due.
- Current production browser не выполняет live-heartbeat gating; helper с такой логикой пока не подключён.
