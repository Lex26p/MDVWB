# MDVWB `buses.json`

`/etc/mdvwb/buses.json` — текущий source of truth для всех шин MDVWB. Файл
принадлежит `mdvwb-manager`: manager проверяет и сохраняет конфигурацию,
публикует её в MQTT и применяет изменения только к затронутым экземплярам
`mdvwb@<bus>.service`. Каждый процесс шины по-прежнему владеет ровно одним
физическим serial port.

Текущая schema version — `1`. Пример canonical-конфигурации:

```json
{
  "version": 1,
  "revision": 4,
  "buses": [
    {
      "id": 1,
      "enabled": true,
      "protocol": "mdv",
      "port": "/dev/ttyRS485-1",
      "addresses": [0, 1, 2]
    },
    {
      "id": 2,
      "enabled": true,
      "protocol": "modbus_rtu",
      "port": "/dev/ttyRS485-2",
      "modbus": {
        "profileId": "vrf_add_controller",
        "baudRate": 9600,
        "dataBits": 8,
        "parity": "none",
        "stopBits": 1
      },
      "addresses": [1, 2]
    }
  ]
}
```

Основные правила:

- `revision` необязателен при чтении, по умолчанию равен `0`; manager всегда
  записывает его в canonical output и использует для optimistic concurrency;
- bus `id` уникален и находится в диапазоне `1..999`;
- `port` — уникальный безопасный абсолютный путь, начинающийся с `/dev/`;
- допустимы только протоколы `mdv` и `modbus_rtu`; отсутствие `protocol` в
  legacy-конфигурации означает `mdv`, но canonical output пишет поле явно;
- адреса MDV находятся в диапазоне `0..63`, Modbus RTU — `1..63`;
- адреса уникальны внутри шины, а enabled-шина должна содержать хотя бы один
  адрес;
- объект `modbus` обязателен только для `modbus_rtu`; его transport settings
  должны соответствовать выбранному установленному профилю;
- неизвестные JSON-поля, duplicate keys, неверные диапазоны и конфликтующие
  serial ports отклоняются;
- шины и адреса canonical serializer сортирует по возрастанию.

Manager поддерживает команды:

```sh
mdvwb-manager validate [buses.json]
mdvwb-manager show [buses.json]
mdvwb-manager summary [buses.json]
mdvwb-manager plan [buses.json]
mdvwb-manager apply [buses.json]
mdvwb-manager mqtt [buses.json]
mdvwb-manager migrate-defaults [buses.json]
```

Если путь не указан, используется `MDVWB_BUSES_CONFIG`, затем
`/etc/mdvwb/buses.json`. `apply` проверяет и синхронизирует systemd, но не
заменяет конфигурационный файл; транзакционная запись и увеличение `revision`
выполняются save endpoint менеджера. Результаты discovery никогда не применяются
автоматически.

Полный контракт, включая MQTT save, rollback и lifecycle служб, описан в
`docs/DEVELOPER.md`.
