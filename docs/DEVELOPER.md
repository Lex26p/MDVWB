# MDVWB: документация для разработчика

> Актуальная версия описываемой архитектуры: **1.2.0**.  
> Документ предназначен для программистов, которые собирают, сопровождают или расширяют MDVWB.  
> Исходный код является окончательным источником истины. При расхождении документа и реализации сначала проверьте текущую ветку проекта.

## 1. Назначение проекта

MDVWB — самостоятельный C++20-драйвер фанкойлов MDV XYE для Wiren Board.

Проект решает четыре основные задачи:

1. Обменивается с фанкойлами по RS-485.
2. Публикует фактические состояния и принимает команды через MQTT.
3. Поддерживает произвольное количество независимых RS-485-шин.
4. Предоставляет менеджер конфигурации и статический веб-интерфейс.

MDVWB не является модулем `wb-mqtt-serial`. Каждый экземпляр драйвера — отдельный процесс, владеющий одним последовательным портом.

## 2. Архитектура верхнего уровня

```text
Браузер
/var/www/mdvwb
      |
      | MQTT over WebSocket: /mqtt
      v
Mosquitto
      |
      +-- mdvwb-manager.service
      |      |
      |      +-- /etc/mdvwb/buses.json
      |      +-- /etc/default/mdvwb-N
      |      +-- systemctl mdvwb@N.service
      |      `-- запуск discovery для выбранной шины
      |
      +-- mdvwb@1.service --> MDVWB --> /dev/ttyRS485-1
      +-- mdvwb@2.service --> MDVWB --> /dev/ttyUSB0
      `-- mdvwb@N.service --> MDVWB --> индивидуальный порт
```

Основные архитектурные решения:

- один процесс обслуживает ровно одну RS-485-шину;
- количество шин не зашито в коде;
- каждая шина работает независимо;
- отказ одной шины не останавливает остальные;
- конфигурация шин хранится в одном JSON-файле;
- systemd отвечает за запуск, перезапуск и журналирование процессов;
- MQTT является единым интерфейсом для устройств, менеджера и веб-страницы.

## 3. Компоненты проекта

Проект собирает два исполняемых файла:

```text
MDVWB
mdvwb-manager
mdvwb-scheduler
```

### 3.1. `MDVWB`

Основной драйвер одной RS-485-шины.

Он:

- открывает последовательный порт;
- последовательно опрашивает настроенные адреса;
- разбирает ответы MDV;
- хранит подтверждённое состояние каждого устройства;
- принимает команды MQTT;
- формирует C3/CC/CD-запросы;
- подтверждает команды последующим C0-чтением;
- публикует фактические состояния;
- публикует metadata устройств Wiren Board;
- поддерживает режим поиска адресов;
- поддерживает аппаратный тест одной команды.

### 3.2. `mdvwb-manager`

Менеджер всех настроенных шин.

Он:

- читает и проверяет `/etc/mdvwb/buses.json`;
- создаёт производные `/etc/default/mdvwb-N`;
- синхронизирует экземпляры `mdvwb@N.service`;
- принимает новую конфигурацию по MQTT;
- запускает и останавливает отдельные шины;
- запускает discovery на выбранной шине;
- публикует статусы и результаты;
- мигрирует старые `/etc/default/mdvwb-N` в общий JSON.

## 4. Структура исходников

Корень проекта содержит только CMake, документацию и каталоги компонентов. C++-исходники разделены на драйвер и менеджер.

### 4.1. `src/driver`

| Файл | Ответственность |
|---|---|
| `src/driver/MDVWB.cpp`, `src/driver/MDVWB.h` | Точка входа, выбор режима запуска, self-test и запуск основного цикла |
| `src/driver/mdv_config.cpp`, `src/driver/mdv_config.h` | Разбор параметров командной строки драйвера |
| `src/driver/mdv_protocol.cpp`, `src/driver/mdv_protocol.h` | Формирование кадров, checksum, проверка и разбор ответов |
| `src/driver/mdv_serial.cpp`, `src/driver/mdv_serial.h` | Последовательный порт, тайминги и выполнение транзакций |
| `src/driver/mdv_device.cpp`, `src/driver/mdv_device.h` | Состояние устройства, кеш полного C3-кадра, ожидающие изменения |
| `src/driver/mdv_driver.cpp`, `src/driver/mdv_driver.h` | Очереди команд, опрос, приоритеты, подтверждающие чтения |
| `src/driver/mdv_discovery.cpp`, `src/driver/mdv_discovery.h` | Трёхпроходный поиск адресов `0..63` |
| `src/driver/mdv_mqtt.cpp`, `src/driver/mdv_mqtt.h` | MQTT-контракт фанкойлов и системного устройства |
| `src/driver/mdv_metadata.cpp`, `src/driver/mdv_metadata.h` | Retained metadata для стандартного интерфейса Wiren Board |
| `src/driver/mdv_mosquitto.cpp`, `src/driver/mdv_mosquitto.h` | Реализация MQTT-клиента на libmosquitto; используется также менеджером |

### 4.2. `src/manager`

| Файл | Ответственность |
|---|---|
| `src/manager/mdv_buses_config.cpp`, `src/manager/mdv_buses_config.h` | Строгий parser/validator/serializer `buses.json` |
| `src/manager/mdv_dashboard_config.cpp`, `src/manager/mdv_dashboard_config.h` | Модель и проверка `dashboard.json`, относительных координат и ссылок на устройства |
| `src/manager/mdvwb_dashboard_upload.cpp`, `src/manager/mdvwb_dashboard_upload.h` | Бинарная загрузка подложки, SHA-256, проверка PNG/JPEG/WebP и временные файлы |
| `src/manager/mdvwb_service_sync.cpp`, `src/manager/mdvwb_service_sync.h` | Планирование и применение изменений systemd |
| `src/manager/mdvwb_manager_cli.cpp`, `src/manager/mdvwb_manager_cli.h` | CLI-команды менеджера |
| `src/manager/mdvwb_manager_main.cpp` | Точка входа `mdvwb-manager` |
| `src/manager/mdvwb_manager_mqtt.cpp`, `src/manager/mdvwb_manager_mqtt.h` | Долгоживущий MQTT-демон менеджера |
| `src/manager/mdvwb_discovery_runner.cpp`, `src/manager/mdvwb_discovery_runner.h` | Запуск `MDVWB --discover` и разбор результата |
| `src/manager/mdvwb_migration.cpp`, `src/manager/mdvwb_migration.h` | Миграция старых per-bus environment-файлов |

### 4.3. Тесты

```text
tests/manager/   — C++-тесты конфигурации, manager MQTT, migration и systemd
tests/web/       — тесты JavaScript-моделей
```

Self-test протокола и драйвера остаётся встроенным в `MDVWB --self-test`.

### 4.4. Развёртывание

```text
deploy/mdvwb@.service
deploy/mdvwb-manager.service
deploy/mdvwb-run
deploy/mdvwb.env
deploy/mdvwb-manager.env
deploy/install_wirenboard.sh
deploy/offline-install.sh
deploy/buses.example.json
```

### 4.5. Веб-интерфейс

```text
www/mdvwb/index.html
www/mdvwb/app.js
www/mdvwb/model.js
www/mdvwb/dashboard-editor.js
www/mdvwb/dashboard-model.js
www/mdvwb/dashboard-placement-editor.js
www/mdvwb/mqtt-client.js
www/mdvwb/styles.css
```

Веб-интерфейс не требует сборки и внешних библиотек.

## 5. CMake-цели

Основные цели:

```text
MDVWB
mdvwb-manager
mdvwb-scheduler
```

Вспомогательные библиотеки:

```text
mdvwb_mosquitto_transport
mdvwb_buses_config
mdvwb_service_sync
mdvwb_discovery_runner
mdvwb_manager_mqtt
mdvwb_manager_cli
mdvwb_scheduler
```

Тестовые исполняемые файлы:

```text
mdvwb_buses_config_test
mdvwb_manager_cli_test
mdvwb_service_sync_test
mdvwb_manager_mqtt_test
mdvwb_discovery_runner_test
mdvwb_migration_test
mdvwb_scheduler_test
```

Self-test протокола запускается самим `MDVWB`:

```text
MDVWB --self-test
```

## 6. Зависимости

Обязательные зависимости:

- компилятор с C++20;
- CMake 3.18 или новее;
- Threads;
- системные средства работы с serial port;
- systemd на целевом Wiren Board.

Для реальной работы с MQTT требуется `libmosquitto`.

Локальная разработка протокола может собираться без неё. Для release-сборки целевого пакета используйте:

```bash
-DMDVWB_REQUIRE_MOSQUITTO=ON
```

При отсутствии libmosquitto такая конфигурация должна завершиться ошибкой, а не собирать неполный production-бинарник.

## 7. Сборка

### 7.1. Windows и Visual Studio CMake

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
ctest --test-dir out/build/x64-debug -C Debug --output-on-failure
node tests/web/mdvwb_web_model_test.mjs
```

### 7.2. Обычная CMake-сборка

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

### 7.3. ARM64

Workflow:

```text
.github/workflows/build-arm64-offline.yml
```

Он должен использовать нативный ARM runner. Возврат к QEMU не рекомендуется: ранее QEMU приводил к сбоям системных Debian-утилит при подготовке пакета.

## 8. Режимы запуска драйвера

### 8.1. Обычный режим

```bash
MDVWB \
  --addresses 1,2,3 \
  --port /dev/ttyRS485-1 \
  --bus 1
```

Обязательные параметры:

```text
--addresses LIST
--port PATH
--bus NUMBER
```

Поддерживается старый позиционный формат:

```bash
MDVWB 1,2,3 /dev/ttyRS485-1 1
```

Новые изменения не должны ломать этот формат без явного решения о прекращении совместимости.

### 8.2. Self-test

```bash
MDVWB --self-test
```

Self-test проверяет ключевые протокольные и внутренние инварианты без реального оборудования.

### 8.3. Только чтение

```bash
MDVWB --addresses 1,2,3 --port /dev/ttyRS485-1 --bus 1 --read-only
```

В этом режиме выполняются только C0-запросы. MQTT и команды записи не используются.

### 8.4. Поиск устройств

```bash
MDVWB --port /dev/ttyRS485-1 --bus 1 --discover
```

Discovery:

- сканирует адреса `0..63`;
- выполняет три полных прохода;
- принимает адрес после хотя бы одного строго корректного C0-ответа;
- не изменяет `buses.json`;
- не перезапускает сервис автоматически.

### 8.5. Тест команды

```bash
MDVWB \
  --addresses 1 \
  --port /dev/ttyRS485-1 \
  --bus 1 \
  --test-command Power=1
```

Поддерживаемые поля:

```text
Power
Mode
Speed
SetTemp
Blinds
Blok
```

Последовательность теста:

1. получить исходное состояние C0;
2. сформировать команду;
3. отправить C3, CC или CD;
4. выполнить подтверждающее C0;
5. вывести результат.

## 9. Конфигурация драйвера

### 9.1. Протокол и тайминги

```text
--master-id NUMBER
--period-ms NUMBER
--response-timeout-ms NUMBER
```

Значения по умолчанию:

```text
master-id = 0
period-ms = 150
response-timeout-ms = 130
```

Все типы транзакций используют один общий start-to-start pacer. Не следует создавать независимые интервалы для чтения, записи, блокировки и разблокировки.

### 9.2. MQTT

```text
--mqtt-host HOST
--mqtt-port PORT
--mqtt-user USER
--mqtt-password PASSWORD
--mqtt-client-id ID
--mqtt-keepalive SEC
--mqtt-reconnect SEC
--mqtt-reconnect-max SEC
```

Значения по умолчанию:

```text
host = 127.0.0.1
port = 1883
client-id = mdvwb-<bus>
keepalive = 60
reconnect = 1
reconnect-max = 10
```

## 10. Транспорт RS-485

Параметры линии:

```text
4800 baud
8 data bits
no parity
1 stop bit
```

Каждый запрос на линии состоит из:

```text
0xFE padding + 16-байтовый MDV-кадр
```

При приёме:

1. Байты вне кадра игнорируются до `0xAA`.
2. После `0xAA` собираются ровно 32 байта.
3. Значение `0x55` внутри payload не завершает кадр.
4. После получения 32 байт проверяются начало, конец, адрес, команда и checksum.

Нельзя использовать поиск первого `0x55` в потоке как способ определить конец ответа.

## 11. Формат MDV-запроса

Запрос имеет ровно 16 байт.

| Индекс | Назначение |
|---:|---|
| 0 | `0xAA` |
| 1 | Команда C0/C3/CC/CD |
| 2 | Адрес устройства `0..63` |
| 3 | ID главного контроллера |
| 4 | `0x80` |
| 5 | ID главного контроллера |
| 6 | Power и Mode |
| 7 | Скорость |
| 8 | SetTemp `16..32` |
| 9 | Дополнительные функции |
| 10 | Таймер включения, сейчас `0` |
| 11 | Таймер выключения, сейчас `0` |
| 12 | Зарезервировано, сейчас `0` |
| 13 | Дополнение кода команды |
| 14 | Checksum |
| 15 | `0x55` |

Условие checksum:

```text
sum(bytes 1..14) mod 256 == 0
```

### 11.1. Byte 6: Power и Mode

```text
bit 0 = Fan
bit 1 = Dry
bit 2 = Heat
bit 3 = Cool
bit 4 = Auto
bit 5 = Mode lock в ответе
bit 7 = Power
```

При формировании C3:

- должен быть выбран ровно один режим;
- Power является независимым битом;
- изменение Power обязано сохранять Mode;
- изменение Mode обязано сохранять Power.

### 11.2. Byte 7: Speed

```text
bit 0 = High
bit 1 = Medium
bit 2 = Low
bit 7 = Auto
```

В команде должна быть выбрана ровно одна скорость.

### 11.3. Byte 9: дополнительные функции

Известные биты:

```text
bit 0 = Eco
bit 1 = Heater
bit 2 = Blinds/Louvers
bit 3 = Fan function
```

Неизвестные и зарезервированные биты не должны устанавливаться без подтверждённого требования протокола.

## 12. Формат MDV-ответа

Ответ имеет ровно 32 байта.

| Индекс | Назначение |
|---:|---|
| 0 | `0xAA` |
| 1 | C0/C3/CC/CD |
| 2 | `0x80` |
| 3 | Master ID |
| 4 | Адрес устройства |
| 5 | Master ID |
| 6 | Неизвестно |
| 7 | Возможности |
| 8 | Power и Mode |
| 9 | Speed |
| 10 | SetTemp |
| 11 | T1 |
| 12 | T2A |
| 13 | T2B |
| 14 | T3 |
| 15 | Ток/потребление |
| 16 | Неизвестно |
| 17 | Timer start |
| 18 | Timer stop |
| 19 | Неизвестно |
| 20 | Дополнительные функции |
| 21 | Статусные биты |
| 22 | Ошибки E0..E7 |
| 23 | Ошибки E8..EF |
| 24 | Защиты P0..P7 |
| 25 | Защиты P8/PF |
| 26 | Ошибки связи 0#..7# |
| 27..29 | Неизвестно |
| 30 | Checksum |
| 31 | `0x55` |

Условие checksum:

```text
sum(bytes 1..30) mod 256 == 0
```

Преобразование температуры:

```text
temperature = raw / 2.0 - 20.0
```

`0xFF` для комнатной температуры означает отсутствие значения.

### 12.1. Различие encoding и decoding

При передаче команды режим и скорость взаимоисключающие.

При разборе ответа допустимы:

- Auto + активный физический режим;
- Auto speed + активная физическая скорость.

Несколько одновременно установленных физических режимов без Auto считаются некорректным ответом. То же правило применяется к физическим скоростям.

## 13. Состояние устройства

Для каждого адреса существует контекст устройства.

Он хранит:

- последнюю подтверждённую C0-модель;
- полный кешированный C3-кадр;
- поля, ожидающие подтверждения;
- состояние очереди команд;
- признаки доступности и ошибок связи;
- ревизию фактических данных для MQTT.

Ключевое правило: полный C3-кадр строится на основе последнего корректного C0.

Изменение одного параметра должно изменять только соответствующий byte/bit и checksum. Остальные значения должны сохраняться.

## 14. Подтверждение команд

C3-ответ нельзя считать новым фактическим состоянием.

Некоторые фанкойлы сразу после команды возвращают старые значения. Поэтому используется следующая схема:

```text
MQTT command
     |
     v
изменение поля кешированного C3
     |
     v
C3 / CC / CD
     |
     v
очередь подтверждающего C0
     |
     v
только корректный C0 обновляет фактическое состояние
```

Правила:

- только C0 синхронизирует подтверждённое состояние;
- старое значение C0 не должно удалять ещё не подтверждённое ожидаемое поле;
- ожидающий флаг снимается после получения требуемого значения;
- C3/CC/CD не публикуются как фактическое состояние;
- Power и Mode всегда сохраняют друг друга.

## 15. Очереди и приоритеты

Приоритет операций:

1. подтверждающее C0;
2. lock/unlock;
3. C3 set;
4. обычный round-robin C0.

За одну итерацию драйвер выполняет одну транзакцию.

MQTT callback не должен непосредственно изменять состояние RS-485-устройства. Callback только помещает разобранную команду во внутреннюю очередь. Обработка контекста устройства выполняется в последовательном рабочем цикле драйвера.

Это предотвращает конкурентное изменение одного `DeviceContext` MQTT-потоком и serial-потоком.

## 16. Round-robin polling

Настроенные адреса опрашиваются циклически.

Пример:

```text
1 -> 2 -> 3 -> 1 -> 2 -> 3 ...
```

Вставка команды не создаёт отдельный параллельный обмен. Она занимает следующую доступную транзакцию согласно приоритетам.

Старт каждой следующей транзакции ограничен общим периодом `period-ms`.

## 17. MQTT-контракт фанкойлов

Имя устройства:

```text
Fan-<bus>_<address>
```

Например:

```text
Fan-2_18
```

### 17.1. Фактические состояния

```text
/devices/Fan-<bus>_<address>/controls/Power
/devices/Fan-<bus>_<address>/controls/Mode
/devices/Fan-<bus>_<address>/controls/Speed
/devices/Fan-<bus>_<address>/controls/SetTemp
/devices/Fan-<bus>_<address>/controls/Temp
/devices/Fan-<bus>_<address>/controls/Blinds
/devices/Fan-<bus>_<address>/controls/Blok
/devices/Fan-<bus>_<address>/controls/Alarm
/devices/Fan-<bus>_<address>/controls/AlarmCode
/devices/Fan-<bus>_<address>/controls/Status
```

Фактические значения:

- публикуются только после корректного C0;
- публикуются с `retain=true`;
- публикуются только при изменении;
- при запуске может выполняться принудительный начальный snapshot;
- не публикуются в `/on1`.

### 17.2. Команды

```text
/devices/Fan-<bus>_<address>/controls/<Control>/on1
```

Поддерживаемые payload:

| Control | Значения |
|---|---|
| `Power` | `0`, `1` |
| `Mode` | `0` Cool, `1` Heat, `2` Dry, `3` Fan, `4` Auto |
| `Speed` | `1` Low, `2` Medium, `3` High, `4` Auto |
| `SetTemp` | целое `16..32` |
| `Blinds` | `0`, `1` |
| `Blok` | `0` unlock, `1` lock |

Команда должна содержать одно целое значение.

Драйвер отклоняет:

- retained-команды;
- неизвестный control;
- неправильное значение;
- адрес другой шины;
- неизвестное устройство;
- устройство без первого валидного C0.

### 17.3. Маппинг состояний

```text
Power: 0/1
Mode: 0 Cool, 1 Heat, 2 Dry, 3 Fan, 4 Auto
Speed: 1 Low, 2 Medium, 3 High, 4 Auto
Alarm: 0 нормально, 1 авария, 2 нет связи
Status: 0 off, 1 cool, 2 heat, 3 dry, 4 fan, 5 auto, 6 alarm, 7 offline
```

`Blok` публикуется отдельно и не влияет на расчёт `Status`.

## 18. Системное MQTT-устройство

Имя:

```text
sist-<bus>
```

Контролы:

```text
/devices/sist-<bus>/controls/Serial
/devices/sist-<bus>/controls/Error
/devices/sist-<bus>/controls/GanGetID
```

`Serial` и `Error` retained и публикуются только при изменении.

`GanGetID` по умолчанию отключён, поскольку публикация каждого опрашиваемого адреса каждые 150 мс создаёт лишний трафик.

Для диагностики его включает:

```text
--publish-poll-address
```

Обычный timeout одного фанкойла должен отражаться через его `Alarm=2` и `Status=7`, а не постоянно перезаписывать системный `Error`.

## 19. MQTT metadata Wiren Board

Драйвер сам создаёт metadata:

```text
/devices/Fan-<bus>_<address>/meta/...
/devices/Fan-<bus>_<address>/controls/<Control>/meta/...
/devices/sist-<bus>/meta/...
```

Это заменяет старый wb-rules-код с `ArrID` и `defineVirtualDevice`.

Metadata и фактические значения публикуются retained.

При удалении адреса или шины необходимо очистить устаревшие retained-топики, иначе устройство продолжит отображаться в Wiren Board после удаления из конфигурации.

В текущей реализации metadata контролов опубликована как readonly. Изменение этой семантики требует проверки стандартного интерфейса Wiren Board и сохранения командного контракта `/on1`.

## 20. Общая конфигурация шин

Канонический путь:

```text
/etc/mdvwb/buses.json
```

Пример:

```json
{
  "version": 1,
  "buses": [
    {
      "id": 1,
      "enabled": true,
      "port": "/dev/ttyRS485-1",
      "addresses": [1, 2, 3]
    },
    {
      "id": 2,
      "enabled": false,
      "port": "/dev/ttyUSB0",
      "addresses": []
    }
  ]
}
```

Правила:

- `version` должно быть равно `1`;
- допустимы только root-поля `version`, `buses`;
- допустимы только bus-поля `id`, `enabled`, `port`, `addresses`;
- `id`: `1..999`;
- ID шин уникальны;
- port уникален;
- port начинается с `/dev/`;
- путь содержит только разрешённые безопасные символы;
- адрес: `0..63`;
- адреса внутри одной шины уникальны;
- активная шина имеет хотя бы один адрес;
- отключённая шина может иметь пустой список;
- неизвестные JSON-поля являются ошибкой;
- serializer сортирует шины и адреса.

`buses.json` является единственным источником истины. Файлы `/etc/default/mdvwb-N` являются производными runtime-файлами.

## 21. CLI менеджера

```text
mdvwb-manager validate [buses.json]
mdvwb-manager show [buses.json]
mdvwb-manager summary [buses.json]
mdvwb-manager plan [buses.json]
mdvwb-manager apply [buses.json]
mdvwb-manager mqtt [buses.json]
mdvwb-manager migrate-defaults [buses.json]
```

Путь разрешается в таком порядке:

1. явный аргумент;
2. `MDVWB_BUSES_CONFIG`;
3. `/etc/mdvwb/buses.json`.

Назначение:

| Команда | Действие |
|---|---|
| `validate` | Проверить JSON |
| `show` | Вывести канонический JSON |
| `summary` | Вывести короткую машинно-читаемую сводку |
| `plan` | Показать будущие изменения без применения |
| `apply` | Создать env-файлы и синхронизировать systemd |
| `mqtt` | Запустить MQTT-демон менеджера |
| `migrate-defaults` | Создать JSON из старых `/etc/default/mdvwb-N` |

`apply`, `mqtt` и `migrate-defaults` требуют root.

Коды завершения:

```text
0 = успех
1 = runtime/manager error
2 = usage/configuration error
```

## 22. Синхронизация systemd

Экземпляр сервиса:

```text
mdvwb@<bus>.service
```

Производный environment-файл:

```text
/etc/default/mdvwb-<bus>
```

Планировщик использует действия:

```text
WriteConfig
RemoveConfig
EnableAndStart
EnableAndRestart
DisableAndStop
EnsureEnabledAndStarted
```

Логика:

| Состояние | Действие |
|---|---|
| Новая активная шина | Записать config, enable и start |
| Изменённая активная шина | Записать config, restart только эту шину |
| Неизменённая активная шина | Убедиться, что enabled и running |
| Отключённая шина | disable и stop |
| Удалённая управляемая шина | stop, disable, удалить производный config |

Менеджер не должен удалять чужие `/etc/default/mdvwb-*`, не распознанные как созданные MDVWB.

Запись файлов должна быть атомарной.

Параметры пользователя не должны передаваться в unrestricted shell-команду. Systemd-операции должны строиться только из проверенного числового ID шины.

## 23. MQTT API менеджера

### 23.1. Конфигурация

```text
/mdvwb/config
/mdvwb/config/set
/mdvwb/config/result
/mdvwb/status
```

Семантика:

| Topic | Retain | Назначение |
|---|---:|---|
| `/mdvwb/config` | да | Текущий канонический JSON |
| `/mdvwb/config/set` | нет | Запрос новой конфигурации |
| `/mdvwb/config/result` | нет | Результат сохранения и применения |
| `/mdvwb/status` | да | Статус менеджера |

Ограничения:

- retained-команда `/set` игнорируется;
- максимальный payload — 64 KiB;
- некорректный JSON не изменяет существующий файл;
- корректный JSON сначала сохраняется атомарно;
- затем выполняется синхронизация сервисов;
- возможен результат `saved=true`, `success=false`, если файл сохранён, но systemd-применение завершилось ошибкой.

### 23.2. Управление шиной

```text
/mdvwb/buses/<id>/start
/mdvwb/buses/<id>/stop
/mdvwb/buses/<id>/restart
/mdvwb/buses/<id>/status/get
/mdvwb/buses/<id>/status
/mdvwb/buses/<id>/result
```

Команды non-retained.

`status` retained, `result` non-retained.

Start и restart запрещены для `enabled=false`.

Stop и status разрешены для любой существующей в конфигурации шины.

Ручная команда не изменяет `buses.json`.

### 23.3. Discovery

```text
/mdvwb/buses/<id>/discovery/start
/mdvwb/buses/<id>/discovery/status
/mdvwb/buses/<id>/discovery/result
```

`status` и `result` retained.

Менеджер:

1. проверяет шину в конфигурации;
2. берёт её port;
3. останавливает только соответствующий сервис;
4. запускает `MDVWB --discover`;
5. публикует найденные адреса;
6. оставляет сервис остановленным;
7. не редактирует конфигурацию.

## 24. Миграция

`migrate-defaults` читает существующие:

```text
/etc/default/mdvwb-1
/etc/default/mdvwb-2
/etc/default/mdvwb-N
```

и формирует начальный `buses.json`.

Установщик должен:

- сохранять существующий непустой `buses.json`;
- выполнять миграцию только при его отсутствии;
- устанавливать example config только при отсутствии и JSON, и legacy-конфигураций;
- не перезаписывать пользовательские настройки при обновлении.

## 25. Статический веб-интерфейс

Веб-страница находится в:

```text
/var/www/mdvwb
```

URL:

```text
http://<WB-address>/mdvwb/
```

Основные свойства:

- статические HTML/CSS/JS;
- отсутствует build step;
- отсутствуют внешние интернет-зависимости;
- MQTT WebSocket — `/mqtt`;
- host определяется из текущего URL;
- карточки шин строятся динамически;
- редактор работает через локальный черновик;
- config отправляется только после явного сохранения;
- управление блокируется при несохранённом черновике;
- discovery results не применяются автоматически;
- `?demo=1` включает локальный демонстрационный режим.

Подробная пользовательская логика должна находиться в `docs/WEB_AND_FANCOILS.md`, а не дублироваться здесь.

## 26. Логирование и ошибки

### 26.1. Драйвер

События должны различать:

- ошибку открытия serial port;
- timeout отдельного устройства;
- некорректный кадр;
- ошибку MQTT;
- отклонённую команду;
- отправленную команду;
- подтверждённое состояние;
- системную ошибку шины.

Timeout одного адреса не означает отказ всей serial-линии.

### 26.2. Менеджер

Ошибки делятся на:

- configuration error;
- usage error;
- filesystem error;
- systemd operation error;
- MQTT error;
- discovery process error.

Публикуемый JSON-результат должен явно показывать, была ли конфигурация сохранена.

## 27. Тесты

Текущий набор CTest:

```text
mdv_protocol_self_test
mdv_buses_config_test
mdvwb_manager_cli_test
mdvwb_service_sync_test
mdvwb_manager_mqtt_test
mdvwb_discovery_runner_test
mdvwb_migration_test
mdvwb_scheduler_test
```

Веб-модель:

```bash
node tests/web/mdvwb_web_model_test.mjs
```

### 27.1. Что тестировать при изменении протокола

- точные bytes кадров;
- checksum;
- fixed frame sizes;
- Power + Mode;
- ровно один командный Mode;
- ровно одна командная Speed;
- Auto + physical response combinations;
- noise перед `0xAA`;
- `0x55` внутри payload;
- старое значение после C3;
- подтверждение через C0.

### 27.2. Что тестировать при изменении MQTT

- правильный topic;
- `/on1`;
- reject retained commands;
- retain фактических значений;
- publish on change;
- правильный numeric mapping;
- очистка stale retained topics;
- bus filtering.

### 27.3. Что тестировать при изменении менеджера

- validator;
- canonical serializer;
- atomic write;
- plan без side effects;
- apply для новой/изменённой/отключённой/удалённой шины;
- защита чужих env-файлов;
- root requirement;
- malformed MQTT payload;
- sync failure после успешного сохранения;
- discovery на выбранной шине;
- migration.

## 28. Добавление нового фактического параметра

Последовательность:

1. Подтвердить byte/bit протокола реальным кадром или документацией.
2. Добавить поле в разобранное состояние `mdv_protocol`.
3. Добавить хранение в `DeviceState`/контексте.
4. Определить, является ли поле только фактическим или также управляемым.
5. Добавить MQTT state topic.
6. Добавить mapping payload.
7. Добавить retained metadata.
8. Добавить change detection.
9. Добавить очистку stale topic при удалении устройства.
10. Добавить self-test и MQTT-тест.
11. Проверить стандартный интерфейс Wiren Board.

## 29. Добавление нового управляемого параметра

Дополнительно к предыдущему разделу:

1. Определить точный byte/bit C3.
2. Убедиться, какие соседние поля надо сохранить.
3. Изменять только соответствующий field кешированного полного C3.
4. Пересчитать checksum.
5. Добавить pending field.
6. Добавить parsing `/on1`.
7. Проверить диапазон payload.
8. Запретить retained-команду.
9. Добавить подтверждение C0.
10. Не публиковать C3 response как факт.
11. Добавить `--test-command`, если параметр требуется для аппаратной диагностики.

## 30. Добавление нового режима менеджера

1. Сначала определить, относится действие к JSON, systemd или discovery.
2. Реализовать чистую логику отдельно от CLI/MQTT.
3. Сделать входные данные строго типизированными.
4. Не передавать произвольный payload в shell.
5. Добавить CLI-тест.
6. Добавить MQTT-тест, если действие доступно браузеру.
7. Определить retain-семантику каждого результата.
8. Очистить obsolete retained topics, если сущность удаляется.
9. Обновить `AGENTS.md`, `DEVELOPER.md` и пользовательскую документацию.

## 31. Добавление поля в `buses.json`

Изменение schema — операция совместимости.

Требуется:

1. определить новую версию schema либо совместимое optional-поле;
2. обновить strict parser;
3. обновить canonical serializer;
4. обновить web model;
5. обновить migration;
6. обновить service sync;
7. добавить тесты unknown/missing/invalid fields;
8. обеспечить обновление старого файла;
9. не перезаписывать рабочую конфигурацию;
10. обновить установочную документацию.

Нельзя просто начать игнорировать неизвестные поля: текущая модель намеренно строгая.

## 32. Поток обычной команды

```text
Browser / WB UI / MQTT client
          |
          | /devices/Fan-B_A/controls/Power/on1
          v
mdv_mosquitto
          |
          v
mdv_mqtt: parse + validate + enqueue
          |
          v
mdv_driver: select queued command
          |
          v
DeviceContext: modify complete cached C3
          |
          v
mdv_serial: paced transaction
          |
          v
C3 response: validate only, not factual state
          |
          v
enqueue C0 confirmation
          |
          v
valid C0
          |
          +-- update confirmed state
          +-- clear pending field when confirmed
          +-- publish changed retained MQTT state
          `-- update WB UI
```

## 33. Поток сохранения конфигурации

```text
Web editor
    |
    | /mdvwb/config/set
    v
mdvwb-manager
    |
    +-- reject retained / oversized / invalid JSON
    |
    +-- canonicalize
    |
    +-- atomic write /etc/mdvwb/buses.json
    |
    +-- build service synchronization plan
    |
    +-- write /etc/default/mdvwb-N
    |
    +-- start/restart/stop only affected services
    |
    +-- clear removed retained topics
    |
    `-- publish /mdvwb/config, /result, bus statuses
```

## 34. Поток discovery

```text
Web / MQTT command
       |
       v
mdvwb-manager
       |
       +-- resolve bus from buses.json
       +-- stop mdvwb@N.service
       +-- execute MDVWB --discover on configured port
       +-- parse result
       +-- publish retained status/result
       `-- leave mdvwb@N.service stopped
```

Остальные `mdvwb@M.service` продолжают работать.

## 35. Инварианты, которые нельзя нарушать

- Request всегда 16 байт.
- Response всегда 32 байта.
- Управление только индивидуальными адресами `0..63`.
- Broadcast `0xFF` не используется.
- Все транзакции используют общий 150 ms pacer.
- Power независим от Mode.
- В команде ровно один Mode.
- В команде ровно одна Speed.
- В ответе допустимы Auto + physical mode/speed.
- Фактическое состояние обновляет только C0.
- Командный суффикс — `/on1`.
- Фактические состояния — base topic, retained.
- Retained-команды отклоняются.
- Один процесс владеет одним serial port.
- Количество шин произвольное.
- Discovery не применяет адреса и не перезапускает шину.
- Web root по умолчанию — `/var/www/mdvwb`.
- Конфигурация пользователя сохраняется при обновлении.
- Старый `ArrID` не должен возвращаться.

## 36. Совместимость и целевая платформа

Целевая среда:

```text
Wiren Board
ARM64
Debian 11 Bullseye
glibc 2.31
systemd
Mosquitto
```

Не следует использовать API или binary dependencies, требующие более новой glibc, если пакет должен работать на текущей целевой системе.

Offline-путь установки обязан оставаться полностью автономным после копирования artifact на контроллер.

## 37. Рекомендации по изменениям

Перед изменением:

1. Прочитать `AGENTS.md`.
2. Проверить текущий исходный код.
3. Определить затрагиваемый контракт.
4. Сохранить обратную совместимость, если её отмена не согласована.
5. Подготовить автоматический тест.
6. При протокольном изменении выполнить аппаратную проверку.
7. Обновить соответствующий документ.

Предпочтения проекта:

- компактные изменения;
- минимальное количество новых файлов;
- отсутствие ненужной инфраструктуры;
- отсутствие скрытых background-процессов;
- понятные systemd units;
- явные MQTT-контракты;
- безопасная обработка конфигурации.

## 38. Контрольный список перед merge

### Код

- [ ] Сборка Windows проходит без предупреждений.
- [ ] Все CTest проходят.
- [ ] Node web-model test проходит при изменении web.
- [ ] libmosquitto обязателен в release-сборке.
- [ ] Не добавлена новая зависимость без необходимости.

### Протокол

- [ ] Frame sizes сохранены.
- [ ] Checksum проверен.
- [ ] Power/Mode/Speed invariants сохранены.
- [ ] C3 не стал фактическим состоянием.
- [ ] Подтверждение C0 работает.

### MQTT

- [ ] State topic и command topic не перепутаны.
- [ ] `/on1` сохранён.
- [ ] State retained.
- [ ] Command non-retained.
- [ ] Numeric mapping не изменился случайно.
- [ ] Stale retained topics очищаются.

### Multi-bus

- [ ] Не появилось ограничение в две шины.
- [ ] Один процесс на один порт.
- [ ] Изменение одной шины не останавливает остальные.
- [ ] `buses.json` остаётся источником истины.
- [ ] Запись атомарная.

### Установка

- [ ] `/var/www/mdvwb` используется по умолчанию.
- [ ] Существующий `buses.json` не перезаписывается.
- [ ] Offline artifact собирается.
- [ ] ARM64/Bullseye совместимость сохранена.
- [ ] Старые сервисы и wb-rules мигрируются безопасно.


## 39. Dashboard configuration backend

Runtime file:

```text
/etc/mdvwb/dashboard.json
```

MQTT contract:

```text
/mdvwb/dashboard/config
/mdvwb/dashboard/config/set
/mdvwb/dashboard/config/result
/mdvwb/dashboard/status
```

The manager creates the default empty dashboard if the file is missing. Current
configuration and status are retained; save requests and results are
non-retained. Payload limit is 1 MiB.

The editor submits the revision it last received. A successful write increments
revision, atomically replaces the file, publishes the canonical JSON and does
not touch systemd services. A mismatched revision is rejected to prevent two
browser tabs from overwriting each other.

References to removed buses or addresses are accepted and counted as
`referenceIssues`. This allows the editor to repair stale markers without losing
layout data after a temporary bus change. Saving `buses.json` republishes the
dashboard status so this warning count remains current.


## 40. Загрузка изображения панели

Backend принимает изображение частями через MQTT. Start содержит `uploadId`,
исходное имя, полный размер, SHA-256 и текущую revision панели. Chunk topic
содержит `uploadId` и последовательный индекс; payload является бинарным.

Ограничения:

- один активный upload;
- 48 KiB на chunk;
- 10 MiB на файл;
- PNG, JPEG или WebP;
- реальные размеры `1..8192` читаются из заголовка;
- SHA-256 проверяется после получения полного файла;
- SVG и retained-команды запрещены;
- finish повторно проверяет dashboard revision.

После проверки temporary file переименовывается в content-addressed
`background-<sha-prefix>.<ext>`, затем атомарно обновляется `dashboard.json`.
Если запись JSON не удалась, новый файл удаляется. Предыдущий управляемый asset
удаляется только после полного успеха. Сервисы шин не затрагиваются.

Environment:

```bash
MDVWB_DASHBOARD_ASSET_DIR="/var/www/fancoils/assets"
```

## 41. Связанные документы

```text
AGENTS.md
docs/DEVELOPER.md
docs/INSTALLATION.md
docs/WEB_AND_FANCOILS.md
README.md
```

Назначение:

- `AGENTS.md` — плотный контекст и ограничения для AI coding agents;
- `DEVELOPER.md` — архитектура и сопровождение кода;
- `INSTALLATION.md` — установка, настройка, эксплуатационные команды и диагностика;
- `WEB_AND_FANCOILS.md` — инструкция пользователя веб-интерфейса и управление фанкойлами;
- `README.md` — краткая точка входа в проект.


## Dashboard marker placement editor

The engineering dashboard editor builds the available device catalog from the applied `/mdvwb/config`. Devices from any configured bus can be placed on one plan. Placements use normalized `x` and `y` coordinates in the `0..1` range, so background zoom and browser resizing do not rewrite positions.

`dashboard-placement-editor.js` owns the catalog, marker drag handling, keyboard movement, selection and property inspector. It updates only the `fans` array in the dashboard draft; saving still uses the existing complete `/mdvwb/dashboard/config/set` payload and revision protection.

One `bus/address` may appear only once. Missing references are rendered as repairable warnings and are retained until the user removes or reassigns them.

## Operational fan-coil page

The separate static application in `www/fancoils/` is served at `/fancoils/` and is independent from the engineering editor UI. It imports `../mdvwb/mqtt-client.js` and `../mdvwb/dashboard-model.js` so MQTT framing and dashboard validation stay shared.

Subscriptions:

```text
/mdvwb/dashboard/config
/mdvwb/dashboard/status
/devices/+/controls/+
```

`www/fancoils/model.js` parses only factual base control topics for known controls. Topics ending in `/on1` are intentionally ignored. `app.js` keeps the latest retained state per `bus:address`, renders only placements with `visible=true`, and uses relative `x/y` coordinates inside the natural background dimensions.

Individual commands use `fanCommandTopic()` and `normalizeFanCommand()` and are published with `retain=false`. The browser does not update its factual cache when publishing. It tracks one pending command per device/control and completes it only when the corresponding factual base topic equals the requested value. Old C3-era state may arrive while a command is pending and must not be treated as failure; the UI waits up to 10 seconds for C0-confirmed state.

The map canvas is rendered at its natural pixel dimensions and scaled as one unit. This causes the background and markers to zoom together while preserving relative positions. The page supports `contain`, `width`, `actual` and `custom` initial scale modes plus temporary operator zoom.


## Working-panel group commands

Group control is implemented in the browser as a plan of individual MQTT operations. The model validates enabled fields, filters offline and unknown devices, skips parameters that already have an outstanding confirmation, and builds one `/devices/Fan-<bus>_<address>/controls/<Control>/on1` operation per target. The browser publishes every operation with `retain=false` and tracks confirmation independently through factual base topics. Supported group fields are `Power`, `Mode`, `Speed`, and `SetTemp`.


### Compact `/fancoils/` layout

The operational page uses a fixed compact top bar and a full-height map viewport. Details and group controls are overlay drawers on the right. It intentionally contains no link to `/mdvwb/`, no editor controls, and no Blinds/Blok user controls. `user-select: none` is scoped to `www/fancoils/styles.css`; the engineering web remains selectable.

<!-- Step 9.2 UI correction: wheel zoom, compact numbered markers, map remains interactive while side drawers are open, direct individual/group selection, no status filter in header. -->

### Map interaction correction

The operational viewport keeps free space around the scaled canvas, centers it after initial load and Fit, and uses scroll-backed pointer panning. Wheel and toolbar zoom preserve the point under the cursor or viewport center. Marker hover tooltips are DOM-built from factual state and show the label, device number, mode, room temperature and speed.


### Compact dashboard editor correction

The engineering dashboard editor is now a single-header, map-first workspace. General settings and background upload share the temporary Parameters drawer, the search field is removed, markers use the approved fan icon, and every placement has a unique editable user number from 1 to 200. Existing configurations without a number are migrated sequentially.

## Editor UI invariants

`DashboardPlacementEditor` must render bus groups dynamically from the manager bus configuration. Do not hard-code bus 1 or bus 2 in production behavior. Device visibility is edited with delegated checkbox events, and map coordinates are snapped with `Math.round(value * 100) / 100`. The administrative bus page and dashboard editor each use a single compact toolbar.


## 41. Multiple independent dashboard panels

The canonical `dashboard.json` schema is version 2. `DashboardCollection` contains a global optimistic-concurrency revision, `defaultPanel`, and up to 64 `DashboardPanel` records. `ParseDashboardCollection()` accepts legacy version 1 and wraps it as `main`; serialization always emits version 2.

The retained `/mdvwb/dashboard/config` payload is the complete collection. The admin editor keeps a collection draft and converts the selected panel to the existing single-panel editing model. Creation, copy, deletion, ID rename and default selection are saved together through `/mdvwb/dashboard/config/set`.

The operator page reads `panel` from the URL and selects that panel from the retained collection. Unknown requested IDs fall back to `defaultPanel` with a visible warning. There is intentionally no operator-side panel selector.

Background upload start carries `panelId`. `ManagerMqttService` validates that the panel exists before accepting bytes and updates only its background after revision and image validation. Content-addressed files may be shared by several panels and must not be removed while still referenced.


### Step 9.8 editor interaction correction

The dashboard editor keeps the 1% grid switch inside **Panel settings**. The editor header contains no zoom controls; wheel input over the map changes only the temporary editor preview scale. The saved opening scale remains the explicit setting in the drawer. Marker rotation is no longer exposed or rendered; legacy `rotation` values are accepted for compatibility and normalized to zero on the next save.


## 42. Schedule configuration backend

Runtime file: `/etc/mdvwb/schedules.json`. `mdv_schedules_config` owns strict parsing, canonical serialization, weekly/once validation, action ranges and cross-reference inspection against both `buses.json` and dashboard collection version 2.

Manager MQTT contract:

```text
/mdvwb/schedules/config
/mdvwb/schedules/config/set
/mdvwb/schedules/config/result
/mdvwb/schedules/status
/mdvwb/schedules/<id>/run
/mdvwb/schedules/<id>/execute
/mdvwb/schedules/<id>/result
```

Configuration saves are complete transactions protected by `revision`, written atomically and limited to 1 MiB. Unlike dashboard placement warnings, broken schedule references reject a save because an executable rule must resolve to an existing visible target. Later bus/dashboard changes can make an existing schedule stale; republishing then reports `state=warning` and `referenceIssues`.

Manual `/run` is validated by the manager and converted to an internal non-retained `/execute` event. Step 11 owns actual timing and command delivery in `mdvwb-scheduler`; do not execute serial or `/on1` commands inside the manager.


## Scheduler architecture

`src/scheduler/mdvwb_scheduler.cpp` owns schedule timing and execution. The MQTT
callback only queues messages; all configuration replacement, command publishing
and confirmation handling occurs in the scheduler loop. Automatic runs are
deduplicated per local minute through the atomic state file. Never use retained
commands, protocol broadcast, C3 replies, or optimistic factual updates.

The scheduler subscribes to:

```text
/mdvwb/schedules/config
/mdvwb/schedules/+/execute
/devices/+/controls/+
```

It publishes command topics with `/on1`, retained global status on
`/mdvwb/scheduler/status`, and non-retained per-run results.
