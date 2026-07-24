# Конфигурация расписаний MDVWB

## Файл

```text
/etc/mdvwb/schedules.json
```

Если файл отсутствует, `mdvwb-manager` создаёт пустую конфигурацию:

```json
{
  "version": 1,
  "revision": 0,
  "schedules": []
}
```

Запись выполняется атомарно. Изменения защищены общей `revision`: редактор должен
отправить ревизию, которую получил последней. Успешное сохранение увеличивает её
на единицу.

## Структура расписания

```json
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
```

### Поля

- `id` — уникальный идентификатор `[A-Za-z0-9_-]`, до 64 символов;
- `name` — отображаемое название, 1–128 байт;
- `enabled` — разрешает автоматическое выполнение; ручной запуск допустим и для
  выключенного расписания;
- `panelId` — пользовательская панель, из которой выбран набор фанкойлов;
- `kind` — `weekly` или `once`;
- `days` — дни недели для `weekly`: `1` — понедельник, `7` — воскресенье;
- `date` — дата `YYYY-MM-DD` для `once`, для `weekly` должна быть пустой;
- `time` — локальное время контроллера в формате `HH:MM`;
- `targets` — непустой список индивидуальных адресов `bus/address`;
- `actions` — как минимум один из параметров `power`, `mode`, `speed`, `setTemp`.

Значения команд совпадают с MQTT-контрактом драйвера:

- `power`: `false`/`true`;
- `mode`: `0 Cool`, `1 Heat`, `2 Dry`, `3 Fan`, `4 Auto`;
- `speed`: `1 Low`, `2 Medium`, `3 High`, `4 Auto`;
- `setTemp`: `16..32`.

## Проверка ссылок

Сохранение отклоняется, если:

- `panelId` не существует;
- шина отсутствует в `buses.json`;
- адрес отсутствует в выбранной шине;
- фанкойл не включён и не виден в выбранной пользовательской панели.

Это исключает сохранение расписания, которое не сможет безопасно выполниться.
Если шина или панель удалены уже после сохранения, `/mdvwb/schedules/status`
переходит в `warning` и сообщает число `referenceIssues`.

## MQTT API

```text
/mdvwb/schedules/config
/mdvwb/schedules/config/set
/mdvwb/schedules/config/result
/mdvwb/schedules/status
/mdvwb/schedules/<id>/run
/mdvwb/schedules/<id>/execute
/mdvwb/schedules/<id>/result
```

- `config` и `status` retained;
- `config/set`, `config/result`, `run`, `execute`, `result` non-retained;
- retained-команды записи и запуска игнорируются;
- максимальный размер `config/set` — 1 MiB.

Ручной `/run` принимает пустой payload, `1`, `run` или `{}`. Менеджер проверяет
расписание и публикует нормализованное событие `/execute`. На шаге 11 отдельная
служба `mdvwb-scheduler` будет выполнять это событие и публиковать окончательный
результат. До установки службы состояние `queued` означает только успешную
проверку и постановку в очередь MQTT.
