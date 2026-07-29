# MDVWB: установка, обновление и эксплуатация на Wiren Board

> Документ описывает текущую версию MDVWB 1.2.0.
> Production-цель: Wiren Board ARM64, Debian 11 Bullseye, systemd, Mosquitto и `libmosquitto.so.1`.
> Команды Linux выполняются от `root`, если явно не указано обратное.

## 1. Назначение

Документ покрывает:

- проверку контроллера;
- рекомендуемую offline-установку;
- обновление без автоматической потери конфигурации;
- создание обязательного backup перед обновлением;
- rollback на предыдущий пакет;
- runtime-файлы и permissions;
- systemd-службы;
- environment-файлы;
- проверку Mosquitto и WebSocket `/mqtt`;
- настройку шин;
- проверку manager, scheduler и driver;
- диагностику;
- частичное и полное удаление.

Описание браузерных страниц:

```text
docs/WEB_AND_FANCOILS.md
```

Подробные schema и runtime-контракты:

```text
docs/DEVELOPER.md
docs/schedules-config.md
```

## 2. Рекомендуемый способ

Production-установка выполняется ARM64 offline package:

```text
MDVWB-arm64-offline.tar.gz
MDVWB-arm64-offline.tar.gz.sha256
```

Причины:

- пакет собирается на ARM64;
- build выполняется внутри Debian 11 Bullseye;
- Mosquitto support обязателен;
- выполняется полный CTest;
- проверяются executable smoke tests;
- пакет содержит четыре executable;
- пакет содержит обе web-страницы;
- включены systemd units и environment templates;
- включены безопасные dashboard/schedules defaults;
- есть внешний checksum архива;
- есть внутренний `SHA256SUMS`.

Интернет на Wiren Board для установки не требуется.

## 3. Текущая версия и executable

```text
MDVWB 1.2.0
```

Устанавливаются:

| Executable | Назначение |
|---|---|
| `/usr/local/bin/MDVWB` | Одна RS-485-шина, polling, commands и factual MQTT |
| `/usr/local/bin/mdvwb-offline` | Retained offline state после stop/crash bus process |
| `/usr/local/bin/mdvwb-manager` | Config writer, systemd sync, dashboard, uploads, discovery |
| `/usr/local/bin/mdvwb-scheduler` | Weekly, once и manual schedules |

Helper:

```text
/usr/local/lib/mdvwb/mdvwb-run
```

Template для generated bus environments:

```text
/usr/local/lib/mdvwb/mdvwb.env
```

## 4. Runtime architecture

```text
Browser
  ├─ /mdvwb/
  └─ /fancoils/
          |
          | MQTT WebSocket /mqtt
          v
      Mosquitto
          |
          ├─ mdvwb-manager.service
          ├─ mdvwb-scheduler.service
          ├─ mdvwb@1.service
          ├─ mdvwb@2.service
          └─ mdvwb@N.service
```

Правила:

- manager является единственным writer JSON configuration;
- scheduler читает все три JSON-файла;
- один `mdvwb@N` владеет одним serial port;
- разные шины работают в отдельных process;
- systemd управляет restart;
- protocol broadcast не используется.

## 5. Устанавливаемые каталоги и файлы

### 5.1. Executable и helper

```text
/usr/local/bin/MDVWB
/usr/local/bin/mdvwb-offline
/usr/local/bin/mdvwb-manager
/usr/local/bin/mdvwb-scheduler
/usr/local/lib/mdvwb/mdvwb-run
/usr/local/lib/mdvwb/mdvwb.env
```

### 5.2. Source-of-truth configuration

```text
/etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
/etc/mdvwb/schedules.json
```

### 5.3. Environment

```text
/etc/default/mdvwb-manager
/etc/default/mdvwb-scheduler
/etc/default/mdvwb-<bus>
```

`/etc/default/mdvwb-<bus>` является generated runtime-файлом.

Его source of truth:

```text
/etc/mdvwb/buses.json
```

### 5.4. Scheduler state

```text
/var/lib/mdvwb/scheduler-state.tsv
```

### 5.5. systemd

```text
/etc/systemd/system/mdvwb@.service
/etc/systemd/system/mdvwb-manager.service
/etc/systemd/system/mdvwb-scheduler.service
```

### 5.6. Web

```text
/var/www/mdvwb/
/var/www/fancoils/
/var/www/fancoils/assets/
```

URLs:

```text
http://<WB-address>/mdvwb/
http://<WB-address>/fancoils/
```

## 6. Что пакет не настраивает

Текущий installer не устанавливает и не изменяет:

- Mosquitto package;
- Mosquitto listener configuration;
- nginx/WB web-server routing;
- TLS certificates;
- firewall;
- controller timezone;
- NTP;
- serial hardware configuration;
- RS-485 wiring.

Ожидается, что Wiren Board уже предоставляет:

```text
Mosquitto TCP endpoint 127.0.0.1:1883
same-origin WebSocket endpoint /mqtt
```

Если `/mqtt` недоступен, static web откроется, но MQTT badge останется offline.

## 7. Требования

### 7.1. Аппаратные

- Wiren Board ARM64;
- RS-485 port либо USB–RS-485;
- MDV XYE line;
- корректные A/B;
- общий GND при необходимости;
- termination и biasing согласно физической линии;
- уникальные device addresses внутри каждой bus.

### 7.2. Runtime

```text
systemd
mosquitto.service
libmosquitto.so.1
root access
```

### 7.3. Offline artifact

Artifact предназначен для:

```text
dpkg architecture = arm64
uname machine = aarch64
```

Другая architecture installer отклоняет.

## 8. Предварительная проверка

### 8.1. Architecture

```bash
dpkg --print-architecture
uname -m
```

Ожидается:

```text
arm64
aarch64
```

### 8.2. OS

```bash
cat /etc/os-release
```

Release package собирается в Debian 11 Bullseye container.

### 8.3. Mosquitto service

```bash
systemctl status mosquitto.service --no-pager
```

### 8.4. Runtime library

```bash
ldconfig -p | grep 'libmosquitto\.so\.1'
```

Без этой library offline installer завершится до изменения installation.

### 8.5. Serial ports

```bash
ls -l /dev/ttyRS485-* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

USB adapter:

```bash
dmesg | tail -n 80
```

Stable USB path:

```bash
find /dev/serial/by-id -maxdepth 1 -type l -ls 2>/dev/null
```

### 8.6. Controller clock

```bash
date
timedatectl status
```

Automatic schedules используют локальное время controller.

## 9. Получение ARM64 artifact

GitHub workflow:

```text
Build ARM64 Offline Package
```

Workflow запускается вручную с:

```text
create_artifact = true
```

Artifact name:

```text
MDVWB-arm64-offline
```

Artifact retention:

```text
30 days
```

В скачанном artifact:

```text
MDVWB-arm64-offline.tar.gz
MDVWB-arm64-offline.tar.gz.sha256
```

## 10. Проверка внешнего checksum на Windows

PowerShell:

```powershell
Get-FileHash "C:\Users\pereverworkki\Downloads\MDVWB-arm64-offline.tar.gz" -Algorithm SHA256
Get-Content "C:\Users\pereverworkki\Downloads\MDVWB-arm64-offline.tar.gz.sha256"
```

Сравните hash.

Checksum-файл использует Linux-формат:

```text
<sha256>  MDVWB-arm64-offline.tar.gz
```

## 11. Копирование на controller

Скопируйте оба файла, например:

```text
/root/MDVWB-arm64-offline.tar.gz
/root/MDVWB-arm64-offline.tar.gz.sha256
```

Допустимы:

- SCP;
- SFTP;
- USB storage;
- Wiren Board file upload;
- локальная защищённая сеть.

## 12. Проверка внешнего checksum на Wiren Board

```bash
cd /root
sha256sum -c MDVWB-arm64-offline.tar.gz.sha256
```

Ожидается:

```text
MDVWB-arm64-offline.tar.gz: OK
```

При `FAILED` не распаковывайте и не запускайте installer.

## 13. Распаковка

```bash
cd /root
rm -rf MDVWB-arm64
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
```

Внутри должен находиться:

```text
offline-install.sh
SHA256SUMS
MDVWB
mdvwb-offline
mdvwb-manager
mdvwb-scheduler
mdvwb-run
mdvwb@.service
mdvwb-manager.service
mdvwb-scheduler.service
www/
docs/
```

## 14. Проверка внутреннего `SHA256SUMS`

```bash
cd /root/MDVWB-arm64
sha256sum -c SHA256SUMS
```

Все строки должны завершиться:

```text
OK
```

Installer повторяет эту проверку автоматически.

## 15. Обязательный backup перед первой установкой или обновлением

Offline installer сохраняет существующие configuration files, но:

```text
не создаёт отдельный backup
не выполняет automatic rollback
```

Создайте backup самостоятельно.

Пример:

```bash
BACKUP="/root/mdvwb-backup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BACKUP"
cp -a /etc/mdvwb "$BACKUP/" 2>/dev/null || true
cp -a /etc/default/mdvwb-manager "$BACKUP/" 2>/dev/null || true
cp -a /etc/default/mdvwb-scheduler "$BACKUP/" 2>/dev/null || true
cp -a /etc/default/mdvwb-* "$BACKUP/" 2>/dev/null || true
cp -a /var/lib/mdvwb "$BACKUP/" 2>/dev/null || true
mkdir -p "$BACKUP/fancoils"
cp -a /var/www/fancoils/assets "$BACKUP/fancoils/" 2>/dev/null || true
```

Зафиксируйте current version:

```bash
/usr/local/bin/MDVWB --version 2>/dev/null || true
systemctl list-units 'mdvwb@*.service' --all --no-pager
```

## 16. Offline installation

```bash
cd /root/MDVWB-arm64
chmod +x offline-install.sh
./offline-install.sh
```

Custom web root:

```bash
MDVWB_WWW_ROOT=/var/www ./offline-install.sh
```

Default:

```text
MDVWB_WWW_ROOT=/var/www
```

Итоговые web paths:

```text
$MDVWB_WWW_ROOT/mdvwb
$MDVWB_WWW_ROOT/fancoils
```

Без особой причины переменную не меняйте.

## 17. Что installer проверяет до установки

Последовательность preflight:

1. effective user ID равен `0`;
2. `dpkg --print-architecture` равен `arm64`;
3. `ldconfig` содержит `libmosquitto.so.1`;
4. присутствуют обязательные package files;
5. внутренний `SHA256SUMS` корректен.

Exit codes:

| Code | Причина |
|---:|---|
| `1` | Не root |
| `2` | Не arm64 |
| `3` | Нет `libmosquitto.so.1` |
| `4` | Package incomplete |
| `5` | Manager не запустился после installation |
| `6` | Scheduler не запустился после installation |

## 18. Что installer останавливает

Перед заменой runtime:

```text
mdvwb-scheduler.service
mdvwb-manager.service
```

После чтения текущих configured bus IDs он останавливает:

```text
mdvwb@<bus>.service
```

для bus IDs из текущего `buses.json`.

Legacy fixed services отключаются:

```text
mdvwb.service
mdvwb-2.service
```

## 19. Что installer заменяет

При каждом запуске заменяются:

```text
/usr/local/bin/MDVWB
/usr/local/bin/mdvwb-offline
/usr/local/bin/mdvwb-manager
/usr/local/bin/mdvwb-scheduler
/usr/local/lib/mdvwb/mdvwb-run
/usr/local/lib/mdvwb/mdvwb.env
/etc/systemd/system/mdvwb@.service
/etc/systemd/system/mdvwb-manager.service
/etc/systemd/system/mdvwb-scheduler.service
```

Static applications копируются поверх:

```text
/var/www/mdvwb/
/var/www/fancoils/
```

Unknown old static files, которых больше нет в package, текущий installer явно не удаляет.

## 20. Что installer сохраняет

Если файл существует, manager/scheduler environment не заменяется:

```text
/etc/default/mdvwb-manager
/etc/default/mdvwb-scheduler
```

Если source-of-truth file существует и не пуст:

```text
/etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
/etc/mdvwb/schedules.json
```

он сохраняется.

Uploaded backgrounds сохраняются:

```text
/var/www/fancoils/assets/
```

Package copy не удаляет assets directory.

## 21. Empty configuration files

Проверка выполняется через:

```text
-s
```

Следовательно, нулевой файл считается отсутствующей конфигурацией.

Для empty:

```text
/etc/mdvwb/buses.json
```

installer пытается legacy migration, затем fallback example.

Для empty dashboard/schedules устанавливаются defaults.

## 22. Default dashboard и schedules

Dashboard:

```json
{
  "version": 2,
  "revision": 0,
  "defaultPanel": "main",
  "panels": [
    {
      "id": "main",
      "title": "Главная панель",
      "background": {
        "file": "",
        "naturalWidth": 0,
        "naturalHeight": 0,
        "defaultScale": 1,
        "fit": "contain"
      },
      "fans": []
    }
  ]
}
```

Schedules:

```json
{
  "version": 1,
  "revision": 0,
  "schedules": []
}
```

Оба default не содержат device references.

## 23. `buses.example.json` fallback

Если legacy migration не нашла конфигурацию, installer устанавливает package example.

Example содержит конкретные enabled buses, ports и addresses.

Перед эксплуатацией обязательно откройте:

```text
/mdvwb/
```

и замените example на реальные:

- bus IDs;
- serial ports;
- addresses;
- enabled state.

Также проверьте:

```bash
cat /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json
```

Не подключайте управление к реальной линии, пока example не проверен.

## 24. Legacy migration

При отсутствии непустого `buses.json` выполняется:

```bash
mdvwb-manager migrate-defaults /etc/mdvwb/buses.json
```

Кандидаты:

```text
/etc/default/mdvwb
/etc/default/mdvwb-<canonical-decimal-id>
```

Migration:

- не выполняет shell content;
- строго разбирает assignments;
- отклоняет duplicate variables;
- требует port и addresses;
- отклоняет ambiguous bus sources;
- проверяет filename/`MDVWB_BUS`;
- не пишет partial target при ошибке.

Если migration завершается ошибкой, installer использует example.

Сообщение migration ошибки остаётся в console и должно быть рассмотрено.

## 25. Legacy cleanup

Installer удаляет:

```text
/etc/systemd/system/mdvwb.service
/etc/systemd/system/mdvwb-2.service
/usr/local/bin/mdvwb-bus
/etc/wb-rules/mdvwb-service-control.js
```

Legacy services:

```text
mdvwb.service
mdvwb-2.service
```

отключаются через `disable --now`.

## 26. Legacy wb-rules fan devices

Installer ищет `/etc/wb-rules/*.js`, содержащий одновременно:

```text
var ArrID =
defineVirtualDevice("Fan-"
```

Такой файл переименовывается:

```text
<original>.disabled-mdvwb
```

Timestamp не добавляется.

Если backup с тем же именем уже существует, `mv` может завершиться ошибкой из-за platform/filesystem behavior.

После scan installer пытается restart:

```text
wb-rules.service
```

Другие wb-rules не отключаются.

## 27. Retained topic cleanup

Если существует command:

```text
mqtt-delete-retained
```

installer удаляет старые retained topics:

```text
/devices/Fan-<bus>_0/# ... /devices/Fan-<bus>_63/#
/devices/sist-<bus>/#
```

для current configured bus IDs.

Ошибки cleanup игнорируются.

Если command отсутствует, шаг пропускается.

Новые drivers затем публикуют current metadata и factual state.

## 28. Self-tests и apply

Installer выполняет:

```bash
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-offline --self-test
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager apply /etc/mdvwb/buses.json
```

Installer не запускает отдельный scheduler self-test; для него выполняется CLI smoke:

```bash
/usr/local/bin/mdvwb-scheduler --help
```

в release workflow до упаковки.

## 29. Запуск служб после installation

```bash
systemctl daemon-reload
systemctl enable --now mdvwb-manager.service
systemctl enable --now mdvwb-scheduler.service
```

`mdvwb-manager apply` синхронизирует per-bus services до запуска manager daemon.

Installer ждёт 3 seconds и проверяет:

```text
mdvwb-manager.service active
mdvwb-scheduler.service active
```

Per-bus services отдельно installer после sleep не проверяет.

Их нужно проверить вручную.

## 30. Первая проверка после installation

### 30.1. Version и executable

```bash
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-offline --self-test
/usr/local/bin/mdvwb-manager --help
/usr/local/bin/mdvwb-scheduler --help
```

### 30.2. Configuration

```bash
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json
```

### 30.3. Services

```bash
systemctl status mdvwb-manager.service --no-pager
systemctl status mdvwb-scheduler.service --no-pager
systemctl list-units 'mdvwb@*.service' --all --no-pager
```

### 30.4. Failed units

```bash
systemctl --failed --no-pager
```

### 30.5. Web files

```bash
test -f /var/www/mdvwb/index.html
test -f /var/www/mdvwb/dashboard-editor.js
test -f /var/www/fancoils/index.html
test -f /var/www/fancoils/schedule-model.js
test -f /var/www/fancoils/scheduler-status-ui.js
test -f /var/www/fancoils/scheduler-status-health.js
```

### 30.6. Configuration files

```bash
ls -l /etc/mdvwb
ls -l /etc/default/mdvwb-*
ls -l /var/lib/mdvwb
ls -l /var/www/fancoils/assets
```

## 31. Открытие web

```text
http://<WB-address>/mdvwb/
http://<WB-address>/fancoils/
```

Используйте завершающий `/`.

IPv6:

```text
http://[IPv6-address]/mdvwb/
http://[IPv6-address]/fancoils/
```

## 32. Mosquitto TCP verification

Subscribe:

```bash
mosquitto_sub -v -t '/mdvwb/#' -t '/devices/Fan-+/controls/+'
```

В другом terminal:

```bash
mosquitto_pub -t '/mdvwb/buses/1/status/get' -m '1'
```

Command должен быть non-retained.

## 33. WebSocket `/mqtt`

Web apps подключаются same-origin:

```text
ws://<host>/mqtt
wss://<host>/mqtt
```

Project не устанавливает WebSocket listener или reverse-proxy route.

Проверка выполняется browser developer tools:

1. откройте Network;
2. выберите WS;
3. найдите `/mqtt`;
4. проверьте status и frames.

Если HTTP открывается, но MQTT offline:

- проверьте `/mqtt`;
- проверьте Mosquitto;
- проверьте WB web routing;
- проверьте authentication;
- проверьте browser mixed-content policy при HTTPS.

## 34. Source-of-truth `buses.json`

```text
/etc/mdvwb/buses.json
```

Редактировать рекомендуется через:

```text
/mdvwb/
```

CLI read-only checks:

```bash
mdvwb-manager validate /etc/mdvwb/buses.json
mdvwb-manager show /etc/mdvwb/buses.json
mdvwb-manager summary /etc/mdvwb/buses.json
mdvwb-manager plan /etc/mdvwb/buses.json
```

Apply:

```bash
mdvwb-manager apply /etc/mdvwb/buses.json
```

`apply` требует root.

## 35. Manager CLI

```text
mdvwb-manager validate [buses.json]
mdvwb-manager show [buses.json]
mdvwb-manager summary [buses.json]
mdvwb-manager plan [buses.json]
mdvwb-manager apply [buses.json]
mdvwb-manager mqtt [buses.json]
mdvwb-manager migrate-defaults [buses.json]
```

Default path:

```text
MDVWB_BUSES_CONFIG
```

либо:

```text
/etc/mdvwb/buses.json
```

## 36. Generated bus environments

Для bus `N`:

```text
/etc/default/mdvwb-N
```

Manager создаёт файл из:

```text
/usr/local/lib/mdvwb/mdvwb.env
```

и заменяет:

```text
MDVWB_BUS
MDVWB_PORT
MDVWB_ADDRESSES
```

Generated marker:

```text
# Managed by mdvwb-manager from buses.json.
```

Mode:

```text
0640
```

Generated environment не редактируйте вручную: следующий apply может его заменить.

## 37. Per-bus environment template

Defaults:

```text
MDVWB_MASTER_ID=0
MDVWB_PERIOD_MS=150
MDVWB_RESPONSE_TIMEOUT_MS=130
MDVWB_MQTT_HOST=127.0.0.1
MDVWB_MQTT_PORT=1883
MDVWB_MQTT_KEEPALIVE=60
MDVWB_MQTT_RECONNECT=1
MDVWB_MQTT_RECONNECT_MAX=10
MDVWB_PUBLISH_POLL_ADDRESS=0
```

Для изменения defaults:

1. измените `/usr/local/lib/mdvwb/mdvwb.env`;
2. выполните `mdvwb-manager apply`;
3. проверьте generated files;
4. restart affected services.

Package update заменяет template.

Сохраняйте custom template отдельно и повторно применяйте осознанно после update.

## 38. Manager environment

```text
/etc/default/mdvwb-manager
```

Создаётся только если отсутствует.

Основные fields:

```text
MDVWB_MQTT_HOST
MDVWB_MQTT_PORT
MDVWB_MQTT_USER
MDVWB_MQTT_PASSWORD
MDVWB_MQTT_KEEPALIVE
MDVWB_MQTT_RECONNECT
MDVWB_MQTT_RECONNECT_MAX

MDVWB_BUSES_CONFIG
MDVWB_DASHBOARD_CONFIG
MDVWB_SCHEDULES_CONFIG
MDVWB_DEFAULT_DIR
MDVWB_ENV_TEMPLATE
MDVWB_BINARY
MDVWB_DASHBOARD_ASSET_DIR
```

После изменения:

```bash
systemctl restart mdvwb-manager.service
```

## 39. Scheduler environment

```text
/etc/default/mdvwb-scheduler
```

Создаётся только если отсутствует.

Основные fields:

```text
MDVWB_MQTT_HOST
MDVWB_MQTT_PORT
MDVWB_MQTT_USER
MDVWB_MQTT_PASSWORD
MDVWB_MQTT_KEEPALIVE
MDVWB_MQTT_RECONNECT
MDVWB_MQTT_RECONNECT_MAX

MDVWB_BUSES_CONFIG
MDVWB_DASHBOARD_CONFIG
MDVWB_SCHEDULES_CONFIG
MDVWB_SCHEDULER_STATE
MDVWB_SCHEDULER_CONFIRM_TIMEOUT
```

Timeout:

```text
1..300 seconds
```

После изменения:

```bash
systemctl restart mdvwb-scheduler.service
```

## 40. systemd: driver instance

Template:

```text
mdvwb@.service
```

Для bus 1:

```text
mdvwb@1.service
```

Runtime:

```text
Environment=MDVWB_CONFIG_FILE=/etc/default/mdvwb-%i
ExecStart=/usr/local/lib/mdvwb/mdvwb-run
ExecStopPost=-/usr/local/lib/mdvwb/mdvwb-run --publish-offline
Restart=on-failure
RestartSec=5
```

`ExecStopPost` запускается и после unexpected exit.

Offline publisher публикует retained offline state перед restart.

## 41. systemd: manager

```text
mdvwb-manager.service
```

Runtime:

```text
EnvironmentFile=-/etc/default/mdvwb-manager
ExecStart=/usr/local/bin/mdvwb-manager mqtt /etc/mdvwb/buses.json
Restart=on-failure
RestartSec=5
Before=mdvwb@1.service mdvwb-scheduler.service
```

`Before=mdvwb@1.service` не перечисляет динамически все IDs.

Фактическая синхронизация остальных instances выполняется manager apply/systemd commands, а не статическим unit ordering.

## 42. systemd: scheduler

```text
mdvwb-scheduler.service
```

Runtime:

```text
EnvironmentFile=-/etc/default/mdvwb-scheduler
ExecStart=/usr/local/bin/mdvwb-scheduler
Restart=on-failure
RestartSec=5
After=network-online.target mosquitto.service mdvwb-manager.service
```

## 43. Service synchronization

`mdvwb-manager apply`:

- создаёт/обновляет generated env;
- enable/start enabled bus;
- restart enabled bus при изменении env;
- disable/stop disabled bus;
- удаляет obsolete managed env после stop;
- не удаляет посторонний env без managed marker.

Проверить план без применения:

```bash
mdvwb-manager plan /etc/mdvwb/buses.json
```

## 44. Ручное управление службами

Manager:

```bash
systemctl restart mdvwb-manager.service
systemctl status mdvwb-manager.service --no-pager
```

Scheduler:

```bash
systemctl restart mdvwb-scheduler.service
systemctl status mdvwb-scheduler.service --no-pager
```

Bus:

```bash
systemctl start mdvwb@1.service
systemctl stop mdvwb@1.service
systemctl restart mdvwb@1.service
systemctl status mdvwb@1.service --no-pager
```

При следующем config apply enabled bus может быть запущена снова.

## 45. Корректный stop sequence для обслуживания

```bash
systemctl stop mdvwb-scheduler.service
systemctl stop mdvwb-manager.service
systemctl stop 'mdvwb@*.service'
```

Shell wildcard в `systemctl` зависит от command interpretation.

Надёжный вариант:

```bash
systemctl list-units 'mdvwb@*.service' --all --no-legend | awk '{print $1}' | xargs -r systemctl stop
```

Stop bus вызывает offline publisher.

## 46. Корректный start sequence

```bash
systemctl start mosquitto.service
mdvwb-manager apply /etc/mdvwb/buses.json
systemctl start mdvwb-manager.service
systemctl start mdvwb-scheduler.service
```

Enabled bus instances запускаются apply.

## 47. Обновление текущей installation

1. Скачайте package для exact release commit.
2. Проверьте внешний checksum.
3. Распакуйте в новый каталог.
4. Проверьте внутренний `SHA256SUMS`.
5. Создайте backup.
6. Сохраните предыдущий package.
7. Выполните новый `offline-install.sh`.
8. Проверьте manager/scheduler.
9. Проверьте все per-bus services.
10. Откройте обе web-страницы.
11. Выполните hardware smoke test.
12. Не удаляйте previous package до завершения проверки.

## 48. Что update не делает

Update не выполняет:

- automatic backup;
- automatic rollback;
- semantic migration dashboard/schedules кроме parser compatibility;
- удаление unknown obsolete web files;
- validation каждого background asset;
- проверку всех bus services после 3-second sleep;
- hardware C0 test;
- проверку WebSocket `/mqtt`;
- проверку controller clock;
- сохранение custom `/usr/local/lib/mdvwb/mdvwb.env`.

Эти проверки выполняются оператором.

## 49. Rollback binaries и web

Сохраните previous known-good package.

Rollback:

```bash
cd /root/MDVWB-arm64-previous
./offline-install.sh
```

Previous installer заменит binaries, units, template и static web.

Current non-empty JSON и assets останутся.

Ограничение:

- новая configuration schema может быть несовместима со старым binary;
- rollback package не восстанавливает старые JSON автоматически;
- scheduler state также остаётся новым.

Для полного rollback нужны backup files.

## 50. Полный rollback configuration

Остановите службы:

```bash
systemctl stop mdvwb-scheduler.service
systemctl stop mdvwb-manager.service
systemctl list-units 'mdvwb@*.service' --all --no-legend | awk '{print $1}' | xargs -r systemctl stop
```

Восстановите:

```text
/etc/mdvwb/
/etc/default/mdvwb-manager
/etc/default/mdvwb-scheduler
/var/lib/mdvwb/
/var/www/fancoils/assets/
```

Затем:

```bash
systemctl daemon-reload
mdvwb-manager validate /etc/mdvwb/buses.json
mdvwb-manager apply /etc/mdvwb/buses.json
systemctl enable --now mdvwb-manager.service
systemctl enable --now mdvwb-scheduler.service
```

Проверьте web, MQTT и hardware.

## 51. Backup matrix

| Данные | Path | Обязательно |
|---|---|---:|
| Buses | `/etc/mdvwb/buses.json` | Да |
| Dashboard | `/etc/mdvwb/dashboard.json` | Да |
| Schedules | `/etc/mdvwb/schedules.json` | Да |
| Backgrounds | `/var/www/fancoils/assets/` | Да |
| Scheduler dedup state | `/var/lib/mdvwb/scheduler-state.tsv` | Желательно |
| Manager MQTT credentials | `/etc/default/mdvwb-manager` | Да при customization |
| Scheduler MQTT credentials | `/etc/default/mdvwb-scheduler` | Да при customization |
| Generated per-bus env | `/etc/default/mdvwb-*` | Диагностически |
| Custom bus template | `/usr/local/lib/mdvwb/mdvwb.env` | Да при customization |
| Previous package | `/root/MDVWB-arm64-*` | До smoke test |

## 52. Permissions

Installer создаёт directories mode:

```text
0755
```

Installed executable/helper:

```text
0755
```

Environment/template/config defaults:

```text
0640
```

systemd units:

```text
0644
```

Web directories:

```text
0755
```

Web files, включая existing files под copied trees:

```text
0644
```

Installer запускается root, поэтому новые files обычно root-owned.

Existing preserved files могут сохранять прежнего owner.

Проверка:

```bash
find /etc/mdvwb -maxdepth 1 -type f -printf '%m %u:%g %p\n'
find /var/www/mdvwb /var/www/fancoils -maxdepth 2 -printf '%m %u:%g %p\n' | head -n 100
```

## 53. MQTT credentials

Credentials могут находиться в:

```text
/etc/default/mdvwb-manager
/etc/default/mdvwb-scheduler
/usr/local/lib/mdvwb/mdvwb.env
/etc/default/mdvwb-<bus>
```

Modes должны исключать read для others.

Не помещайте password:

- в `buses.json`;
- в dashboard;
- в schedules;
- в browser JavaScript;
- в Git repository.

WebSocket authentication определяется существующей конфигурацией controller, не MDVWB installer.

## 54. Logs

Manager:

```bash
journalctl -u mdvwb-manager.service -n 150 --no-pager
journalctl -u mdvwb-manager.service -f
```

Scheduler:

```bash
journalctl -u mdvwb-scheduler.service -n 150 --no-pager
journalctl -u mdvwb-scheduler.service -f
```

Bus:

```bash
journalctl -u mdvwb@1.service -n 200 --no-pager
journalctl -u mdvwb@1.service -f
```

All MDVWB:

```bash
journalctl -u mdvwb-manager.service -u mdvwb-scheduler.service -u 'mdvwb@*.service' -n 300 --no-pager
```

## 55. Service restart loop

Проверьте:

```bash
systemctl status <unit> --no-pager
journalctl -u <unit> -n 200 --no-pager
```

Частые причины:

- invalid config;
- отсутствует executable;
- нет `libmosquitto.so.1`;
- serial port отсутствует;
- serial port занят;
- неправильный MQTT host/credentials;
- generated env unreadable;
- scheduler references invalid;
- target bus disabled.

## 56. Serial port занят

```bash
fuser -v /dev/ttyRS485-1
lsof /dev/ttyRS485-1 2>/dev/null
```

Один serial port должен принадлежать одному driver process.

Проверьте duplicate ports в `buses.json`.

## 57. Manager не запускается

```bash
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager plan /etc/mdvwb/buses.json
cat /etc/default/mdvwb-manager
journalctl -u mdvwb-manager.service -n 200 --no-pager
```

Проверьте permissions:

```bash
namei -l /etc/mdvwb/buses.json
namei -l /var/www/fancoils/assets
```

## 58. Scheduler не запускается

```bash
/usr/local/bin/mdvwb-scheduler --help
cat /etc/default/mdvwb-scheduler
cat /etc/mdvwb/schedules.json
cat /etc/mdvwb/dashboard.json
journalctl -u mdvwb-scheduler.service -n 200 --no-pager
```

Проверьте:

- JSON;
- panel;
- visible targets;
- buses/addresses;
- target bus enabled;
- state directory;
- MQTT.

## 59. Driver не запускается

```bash
cat /etc/default/mdvwb-1
/usr/local/lib/mdvwb/mdvwb-run
journalctl -u mdvwb@1.service -n 200 --no-pager
```

Не запускайте `mdvwb-run` вручную, пока service active и владеет port.

Для безопасной диагностики:

```bash
systemctl stop mdvwb@1.service
MDVWB_CONFIG_FILE=/etc/default/mdvwb-1 /usr/local/lib/mdvwb/mdvwb-run
```

После проверки:

```bash
systemctl start mdvwb@1.service
```

## 60. Проверка offline publisher

Откройте subscription:

```bash
mosquitto_sub -v \
  -t '/devices/Fan-1_1/controls/Alarm' \
  -t '/devices/Fan-1_1/controls/Status'
```

Остановите test bus:

```bash
systemctl stop mdvwb@1.service
```

Ожидается retained:

```text
Alarm=2
Status=7
```

Затем:

```bash
systemctl start mdvwb@1.service
```

После новых C0 responses factual state обновится.

## 61. Discovery после installation

Используйте `/mdvwb/`.

Discovery:

- останавливает selected bus;
- сканирует `0..63` три прохода;
- не применяет addresses автоматически;
- не запускает bus обратно;
- same bus не сканируется параллельно;
- разные buses могут сканироваться одновременно.

После результата:

1. перенесите addresses в draft;
2. сохраните config;
3. включите bus;
4. проверьте service.

## 62. Hardware smoke test

Минимум одна test target:

1. C0 factual state приходит;
2. Power command подтверждается C0;
3. Mode command подтверждается C0;
4. Speed command подтверждается C0;
5. SetTemp подтверждается C0 либо корректно уходит в bounded timeout;
6. stop bus публикует offline;
7. restart восстанавливает polling;
8. group command меняет только выбранные fields;
9. manual schedule достигает terminal result;
10. automatic schedule выполняется по controller time.

## 63. Web open, MQTT offline

Проверьте:

```bash
systemctl status mosquitto.service --no-pager
systemctl status mdvwb-manager.service --no-pager
```

Broker TCP:

```bash
mosquitto_sub -C 1 -v -t '/mdvwb/status'
```

Browser:

- developer tools;
- Network;
- WS;
- `/mqtt`.

Project не содержит automatic repair WebSocket route.

## 64. Dashboard background не записывается

Проверьте:

```bash
ls -ld /var/www/fancoils/assets
touch /var/www/fancoils/assets/.mdvwb-write-test
rm -f /var/www/fancoils/assets/.mdvwb-write-test
journalctl -u mdvwb-manager.service -n 200 --no-pager
```

Manager работает root в текущем unit.

Проверьте filesystem read-only и свободное место:

```bash
mount | grep ' / '
df -h
df -i
```

## 65. Configuration recovery

### 65.1. Invalid buses

```bash
mdvwb-manager validate /etc/mdvwb/buses.json
```

Восстановите backup либо исправьте через offline editing.

Затем:

```bash
mdvwb-manager apply /etc/mdvwb/buses.json
systemctl restart mdvwb-manager.service
```

### 65.2. Invalid dashboard

Остановите manager только при ручном file repair:

```bash
systemctl stop mdvwb-manager.service
```

Восстановите backup.

Затем:

```bash
systemctl start mdvwb-manager.service
```

### 65.3. Invalid schedules

```bash
systemctl stop mdvwb-scheduler.service
systemctl stop mdvwb-manager.service
```

Восстановите file, затем:

```bash
systemctl start mdvwb-manager.service
systemctl start mdvwb-scheduler.service
```

## 66. Disk space

```bash
df -h /
df -i /
du -sh /var/www/fancoils/assets
journalctl --disk-usage
```

Background upload maximum одного файла:

```text
10 MiB
```

Но backups и несколько assets требуют дополнительное место.

## 67. Online installer из source tree

Script:

```bash
deploy/install_wirenboard.sh
```

Он:

- устанавливает build dependencies через apt;
- выполняет Release build;
- выполняет CTest;
- устанавливает четыре executable;
- устанавливает units;
- устанавливает manager/scheduler env только при отсутствии;
- копирует `/mdvwb/`;
- выполняет migration/apply;
- запускает manager и scheduler.

### Текущее ограничение online installer

Текущий script не копирует:

```text
www/fancoils/
```

и не устанавливает явно:

```text
dashboard.default.json
schedules.default.json
```

Также он не выполняет полный offline installer cleanup и checksum workflow.

Поэтому для production и fresh controller используйте offline ARM64 package.

Online script рассматривайте как developer convenience до отдельного исправления deployment code.

## 68. Online build requirements

```bash
apt-get update
apt-get install -y build-essential cmake libmosquitto-dev
```

Запуск:

```bash
chmod +x deploy/install_wirenboard.sh
deploy/install_wirenboard.sh
```

Custom build dir:

```bash
MDVWB_BUILD_DIR=/root/mdvwb-build deploy/install_wirenboard.sh
```

## 69. Ручная build

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMDVWB_REQUIRE_MOSQUITTO=ON
cmake --build build-release --parallel 1
ctest --test-dir build-release --output-on-failure
```

Smoke:

```bash
./build-release/MDVWB --version
./build-release/MDVWB --self-test
./build-release/mdvwb-offline --self-test
./build-release/mdvwb-manager validate config/buses.example.json
./build-release/mdvwb-scheduler --help
```

## 70. Ручная installation

Ручной путь должен повторять offline installer:

- четыре executable;
- helper;
- template;
- три units;
- manager/scheduler env;
- three JSON files;
- обе web applications;
- assets directory;
- state directory;
- `daemon-reload`;
- manager validate/apply;
- enable manager/scheduler;
- smoke tests.

Не устанавливайте только `MDVWB` и manager: dashboard, schedules, offline state и web окажутся неполными.

## 71. Частичное удаление: остановить MDVWB, сохранить данные

```bash
systemctl disable --now mdvwb-scheduler.service
systemctl disable --now mdvwb-manager.service
systemctl list-units 'mdvwb@*.service' --all --no-legend | awk '{print $1}' | xargs -r systemctl disable --now
```

Оставьте:

```text
/etc/mdvwb/
/var/lib/mdvwb/
/var/www/fancoils/assets/
```

## 72. Удаление executable и units с сохранением config

```bash
rm -f /usr/local/bin/MDVWB
rm -f /usr/local/bin/mdvwb-offline
rm -f /usr/local/bin/mdvwb-manager
rm -f /usr/local/bin/mdvwb-scheduler
rm -rf /usr/local/lib/mdvwb
rm -f /etc/systemd/system/mdvwb@.service
rm -f /etc/systemd/system/mdvwb-manager.service
rm -f /etc/systemd/system/mdvwb-scheduler.service
systemctl daemon-reload
```

Generated `/etc/default/mdvwb-*` можно сохранить для диагностики или удалить после backup.

## 73. Полное удаление данных

Перед выполнением создайте backup.

```bash
rm -rf /etc/mdvwb
rm -f /etc/default/mdvwb-manager
rm -f /etc/default/mdvwb-scheduler
rm -f /etc/default/mdvwb-*
rm -rf /var/lib/mdvwb
rm -rf /var/www/mdvwb
rm -rf /var/www/fancoils
```

Это удаляет configuration, scheduler state и uploaded backgrounds.

## 74. Release artifact internals

Package workflow:

- runner `ubuntu-24.04-arm`;
- native machine `aarch64`;
- build container `debian:bullseye`;
- CMake Release;
- `MDVWB_REQUIRE_MOSQUITTO=ON`;
- `-static-libstdc++ -static-libgcc`;
- full CTest;
- executable smoke;
- internal checksums;
- tar.gz;
- external checksum.

Runtime всё равно требует:

```text
libmosquitto.so.1
```

libstdc++ и libgcc статически link flags не делают Mosquitto static.

## 75. Validate workflow

Workflow:

```text
Validate MDVWB
```

Проверяет:

- shell syntax;
- default JSON;
- required web files;
- Release build;
- full CTest;
- driver version/self-test;
- manager validation;
- scheduler help.

Docs-only changes не входят в его automatic path filter.

Для release commit, содержащего только docs, workflow запускайте вручную либо проверяйте последний code commit отдельно.

## 76. Безопасность

- Installer запускайте только после checksum.
- Сохраняйте previous package.
- Делайте backup до update.
- Не храните MQTT password в web.
- Не публикуйте retained command topics.
- Не запускайте два process на одном serial port.
- Не выполняйте blind rollback без проверки schema compatibility.
- Не удаляйте scheduler state в due minute.
- Не удаляйте assets без dashboard reference check.
- Не считайте active manager доказательством исправности per-bus services.
- Не считайте open HTTP page доказательством исправности `/mqtt`.

## 77. Минимальный post-install checklist

```text
[ ] Architecture arm64
[ ] External checksum OK
[ ] Internal SHA256SUMS OK
[ ] Backup создан
[ ] MDVWB --version = 1.2.0
[ ] MDVWB --self-test OK
[ ] mdvwb-offline --self-test OK
[ ] buses.json validate OK
[ ] manager active
[ ] scheduler active
[ ] all expected mdvwb@N checked
[ ] /mdvwb/ opens
[ ] /fancoils/ opens
[ ] /mqtt connected
[ ] controller clock correct
[ ] factual C0 visible
[ ] command confirmed
[ ] offline publisher checked
[ ] manual schedule terminal result
[ ] previous package retained
```

## 78. Installation invariants

- Offline package является production path.
- Installer требует root, arm64 и `libmosquitto.so.1`.
- Checksums проверяются до installation.
- Existing non-empty JSON сохраняются.
- Existing manager/scheduler env сохраняются.
- Existing uploaded assets сохраняются.
- Installer не создаёт backup.
- Installer не выполняет rollback.
- Binaries, units, helper и template заменяются.
- Web files копируются поверх.
- Manager apply синхронизирует bus services.
- Manager и scheduler проверяются после 3-second delay.
- Per-bus services требуют отдельной проверки.
- `/mqtt` должен существовать вне MDVWB deployment.
- Controller local time определяет automatic schedules.
- Previous package и config backup необходимы до завершения smoke test.
