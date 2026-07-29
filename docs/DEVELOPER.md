# MDVWB: документация драйвера, протокола и MQTT

> Актуальная версия CMake-проекта: **1.2.0**.
> Этот документ описывает фактическую реализацию `MDVWB` и `mdvwb-offline`: протокол MDV XYE, serial transport, модель состояния устройства, очереди команд и MQTT-контракт.
> При расхождении документа с исходным кодом источником истины являются текущие файлы `src/driver/` и профильные тесты.

## 1. Назначение драйвера

`MDVWB` — самостоятельный C++20-драйвер одной физической RS-485-шины фанкойлов MDV XYE.

Один процесс `MDVWB`:

- открывает ровно один последовательный порт;
- последовательно опрашивает настроенные адреса `0..63`;
- принимает команды MQTT только для своей шины;
- формирует C3, CC и CD;
- подтверждает изменения последующим C0;
- публикует только фактические значения, полученные из корректного C0;
- создаёт retained metadata устройств Wiren Board;
- не управляет `buses.json` и systemd самостоятельно.

Количество шин не ограничено кодом драйвера. Для каждой физической шины менеджер запускает отдельный экземпляр:

```text
mdvwb@1.service
mdvwb@2.service
...
mdvwb@N.service
```

Разделение по процессам является архитектурным инвариантом: один процесс владеет одним serial port, а сбой одной шины не должен останавливать остальные.

## 2. Исполняемые файлы, относящиеся к шине

### 2.1. `MDVWB`

Основной процесс обмена по RS-485 и MQTT.

Поддерживает:

```text
--help
--version
--self-test
обычный режим
--read-only
--discover
--test-command NAME=VALUE
```

### 2.2. `mdvwb-offline`

Короткоживущий MQTT-публикатор, который запускается из `ExecStopPost` после остановки или аварийного завершения `MDVWB`.

Он публикует для всех адресов шины:

```text
Alarm = 2
Status = 7
```

и для системного устройства:

```text
Serial = Порт закрыт
```

Все эти публикации retained.

## 3. Карта исходников драйвера

| Файл | Ответственность |
|---|---|
| `src/driver/MDVWB.cpp`, `MDVWB.h` | Точка входа, режимы запуска, основной цикл и встроенный self-test |
| `src/driver/mdv_config.cpp`, `.h` | Разбор CLI, значения по умолчанию и проверка сочетаний параметров |
| `src/driver/mdv_protocol.cpp`, `.h` | Формирование запросов, checksum, разбор ответа и сборка фиксированного кадра из потока |
| `src/driver/mdv_serial.cpp`, `.h` | Windows/POSIX serial port, 4800 8N1, общий transaction pacer |
| `src/driver/mdv_device.cpp`, `.h` | Подтверждённое C0-состояние, полный cached C3 и pending fields |
| `src/driver/mdv_driver.cpp`, `.h` | Round-robin, приоритетные очереди, C3/CC/CD, C0 confirmation и лимиты повторов |
| `src/driver/mdv_discovery.cpp`, `.h` | Трёхпроходный поиск корректно отвечающих адресов `0..63` |
| `src/driver/mdv_mqtt.cpp`, `.h` | Входящие команды, factual state, Alarm/Status и системное MQTT-устройство |
| `src/driver/mdv_metadata.cpp`, `.h` | Retained metadata устройств и контролов Wiren Board |
| `src/driver/mdv_mosquitto.cpp`, `.h` | Асинхронный MQTT transport на libmosquitto |
| `src/driver/mdv_bounded_queue.h` | Ограниченная FIFO с объединением последнего значения по логическому ключу |
| `src/driver/mdv_offline.cpp` | Retained offline-состояние после завершения процесса шины |

## 4. Режимы запуска

### 4.1. Обычный режим

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

Для production-конфигурации номера шин находятся в диапазоне `1..999`.

Сохраняется совместимость со старым позиционным запуском:

```bash
MDVWB 1,2,3 /dev/ttyRS485-1 1
```

В обычном режиме требуются:

- реальный serial port;
- сборка с libmosquitto;
- хотя бы один уникальный адрес `0..63`;
- первый корректный C0 перед любой командой записи конкретному устройству.

### 4.2. Self-test

```bash
MDVWB --self-test
```

Self-test не требует реального фанкойла и проверяет:

- точные C0/C3/CC/CD кадры;
- checksum;
- fixed-size framing;
- Auto + physical mode/speed;
- кеш полного C3;
- сохранение desired state при старом C0;
- serial timing;
- discovery;
- round-robin;
- command confirmation;
- MQTT routing, factual publication и metadata;
- CLI-валидацию.

### 4.3. Только чтение

```bash
MDVWB \
  --addresses 1,2,3 \
  --port /dev/ttyRS485-1 \
  --bus 1 \
  --read-only
```

В этом режиме:

- выполняются только C0;
- MQTT не запускается;
- C3, CC и CD не формируются;
- фактические значения печатаются в консоль в формате MQTT topic/value.

### 4.4. Discovery

```bash
MDVWB \
  --port /dev/ttyRS485-1 \
  --bus 1 \
  --discover
```

Discovery:

- не принимает `--addresses`;
- не запускает MQTT;
- отправляет только C0;
- сканирует `0..63` по порядку;
- выполняет ровно три прохода;
- считает адрес найденным после хотя бы одного полностью корректного C0;
- игнорирует timeout, повреждённый кадр и ответ с другой командой;
- выводит машинно-читаемую итоговую строку:

```text
FOUND_ADDRESSES=1,3,18
```

Сам `MDVWB --discover` не изменяет конфигурацию и не управляет systemd. Остановку выбранного сервиса и публикацию результата выполняет `mdvwb-manager`.

### 4.5. Аппаратный тест одной команды

```bash
MDVWB \
  --addresses 1 \
  --port /dev/ttyRS485-1 \
  --bus 1 \
  --test-command SetTemp=24
```

Поддерживаемые команды:

| Имя | Допустимые значения |
|---|---|
| `Power` | `0`, `1` |
| `Mode` | `0..4` |
| `Speed` | `1..4` |
| `SetTemp` | `16..32` |
| `Blinds` | `0`, `1` |
| `Blok` | `0`, `1` |

Режим требует ровно один адрес.

Последовательность:

1. До 20 попыток получить первый корректный C0.
2. При отсутствии C0 завершиться без отправки write-команды.
3. Изменить cached command state.
4. Выполнить C3, CC или CD.
5. Выполнить подтверждающий C0.
6. Завершиться успешно только при совпадении фактического состояния.
7. Прекратить тест после 40 transaction slots, если подтверждение не получено.

### 4.6. Несовместимые параметры

Запрещены сочетания:

```text
--discover + --addresses
--discover + --read-only
--discover + --test-command
--read-only + --test-command
--test-command с несколькими адресами
```

## 5. Параметры транспорта и MQTT

### 5.1. MDV и serial

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

Ограничения:

- master ID: `0..63`;
- transaction period: не менее `150 ms`;
- response timeout: положительный и строго меньше transaction period.

### 5.2. MQTT

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

Пароль без имени пользователя отклоняется.

### 5.3. Диагностическая публикация адреса

```text
--publish-poll-address
```

Флаг включает публикацию:

```text
/devices/sist-<bus>/controls/GanGetID
```

При обычной работе он выключен, потому что публикация адреса каждой транзакции создаёт лишний MQTT-трафик.

## 6. Физический serial transport

Параметры линии:

```text
4800 baud
8 data bits
no parity
1 stop bit
no hardware flow control
```

Windows-порты нормализуются:

```text
COM3 -> \\.\COM3
```

Linux-пути используются без изменения:

```text
/dev/ttyRS485-1
/dev/ttyUSB0
/dev/serial/by-id/...
```

Перед каждой транзакцией transport:

1. ждёт разрешённого start time;
2. очищает входной serial buffer;
3. добавляет один transport padding byte `0xFE`;
4. отправляет весь запрос;
5. читает поток до response deadline;
6. передаёт байты в `ResponseFrameCollector`;
7. возвращает первый собранный 32-байтовый кадр;
8. при отсутствии кадра возвращает timeout.

Запрос на линии имеет размер 17 байт:

```text
0xFE + 16-байтовый MDV request
```

## 7. Общий transaction pacer

Все запросы одной шины используют один экземпляр `TransactionPacer`.

Это относится к:

```text
C0 read
C3 set
CC lock
CD unlock
```

Период является start-to-start:

```text
start(request N+1) >= start(request N) + period
```

Нельзя создавать отдельные таймеры для чтения, записи или блокировки. Любая операция занимает следующий разрешённый transaction slot.

По умолчанию:

```text
response deadline = start + 130 ms
next allowed start = start + 150 ms
```

## 8. Request frame

MDV request имеет ровно 16 байт.

| Byte | Текущая реализация |
|---:|---|
| 0 | `0xAA` |
| 1 | `0xC0`, `0xC3`, `0xCC` или `0xCD` |
| 2 | Адрес устройства `0..63` |
| 3 | Master ID `0..63` |
| 4 | `0x80` |
| 5 | Master ID |
| 6 | Power + Mode для C3, иначе `0` |
| 7 | Speed для C3, иначе `0` |
| 8 | SetTemp для C3, иначе `0` |
| 9 | Known additional functions для C3, иначе `0` |
| 10 | `0` |
| 11 | `0` |
| 12 | `0` |
| 13 | Побитовое дополнение command byte |
| 14 | Checksum |
| 15 | `0x55` |

### 8.1. Checksum запроса

Реализованный инвариант:

```text
sum(bytes 1..14) mod 256 == 0
```

Checksum вычисляется как дополнение до нуля суммы bytes `1..13`.

При изменении любого поля C3 checksum пересчитывается сразу.

### 8.2. Byte 6: Power и Mode

```text
bit 0 = Fan
bit 1 = Dry
bit 2 = Heat
bit 3 = Cool
bit 4 = Auto
bit 7 = Power
```

Для исходящего C3:

- установлен ровно один Mode;
- Power является независимым;
- изменение Power сохраняет Mode;
- изменение Mode сохраняет Power;
- другие mode bits не допускаются.

### 8.3. Byte 7: Speed

```text
bit 0 = High
bit 1 = Medium
bit 2 = Low
bit 7 = Auto
```

Исходящий C3 содержит ровно одну допустимую скорость.

### 8.4. Byte 8: SetTemp

Допустимый диапазон:

```text
16..32
```

### 8.5. Byte 9: additional functions

Текущая реализация признаёт младшие четыре бита как known-function mask:

```text
bits 0..3 = сохраняемые known function bits
bit 2 = Blinds
```

Имена и отдельная семантика bits 0, 1 и 3 в текущем коде не определены. Они синхронизируются из C0 и сохраняются при изменении `Blinds`, но не имеют отдельных MQTT controls.

Reserved bits за пределами `0..3` не должны попадать в исходящий C3.

## 9. Response frame

MDV response имеет ровно 32 байта.

| Byte | Текущая реализация |
|---:|---|
| 0 | Должен быть `0xAA` |
| 1 | C0/C3/CC/CD |
| 2 | Должен быть `0x80` |
| 3 | Master ID |
| 4 | Адрес устройства |
| 5 | Master ID |
| 6 | Не интерпретируется текущей моделью |
| 7 | Не интерпретируется текущей моделью |
| 8 | Power, Mode, Auto/active Mode и lock bit |
| 9 | Auto/physical Speed |
| 10 | SetTemp |
| 11 | T1 room temperature |
| 12..19 | Не публикуются текущим драйвером |
| 20 | Known additional functions |
| 21 | Status bits |
| 22 | Errors E0..E7 |
| 23 | Errors E8..EF |
| 24 | Protections P0..P7 |
| 25 | Protections P8/PF |
| 26 | Communication error bits |
| 27..29 | Не интерпретируются |
| 30 | Checksum |
| 31 | Должен быть `0x55` |

### 9.1. Проверка ответа

Кадр считается корректным только при выполнении всех условий:

- начало `0xAA`;
- конец `0x55`;
- известная команда;
- byte 2 равен `0x80`;
- оба master ID совпадают с ожидаемым;
- адрес находится в `0..63`;
- адрес совпадает с запросом, если он был указан;
- checksum корректна;
- Mode/Speed имеют допустимое сочетание.

Реализованный checksum-инвариант:

```text
sum(bytes 1..30) mod 256 == 0
```

### 9.2. Сборка кадра из потока

`ResponseFrameCollector`:

- игнорирует всё до первого `0xAA`;
- после `0xAA` собирает ровно 32 байта;
- не завершает кадр по `0x55` внутри payload;
- проверяет `0x55` только в byte 31;
- при неправильном конце ищет следующий `0xAA` внутри уже собранных данных и пытается ресинхронизироваться.

## 10. Decoding Mode и Speed

### 10.1. Mode

В ответе допускается:

```text
Auto
Auto + один физический Mode
один физический Mode
нулевой Mode
```

При `Auto + Cool`:

```text
mode = Auto
activeMode = Cool
```

Несколько физических режимов одновременно являются ошибкой.

Lock хранится отдельно:

```text
modeLocked = byte8 bit5
```

### 10.2. Speed

В ответе допускается:

```text
Auto speed
Auto speed + одна физическая Speed
одна физическая Speed
нулевая Speed
```

При `Auto + Low`:

```text
fanSpeed = Auto
activeFanSpeed = Low
```

Несколько физических скоростей одновременно являются ошибкой.

### 10.3. T1

Преобразование:

```text
roomTemperature = raw / 2.0 - 20.0
```

Значение:

```text
0xFF
```

означает отсутствие T1. В этом случае `roomTemperature` остаётся пустым.

## 11. Confirmed state и cached command state

Для каждого адреса `DeviceContext` хранит две разные сущности.

### 11.1. Actual state

`actualState` — последний корректный C0.

Только C0 может вызвать:

```text
SynchronizeReadState()
```

C3/CC/CD response запрещено передавать в этот метод.

### 11.2. Cached C3

`setFrame` — полный 16-байтовый C3, который можно безопасно отправить после изменения одного поля.

Он создаётся после первого корректного C0.

Если выключенный фанкойл возвращает нулевой Mode, Speed или SetTemp, используются безопасные значения:

```text
Mode = Auto
Speed = Auto
SetTemp = 21
```

Эти fallback нужны только для валидного исходящего C3 и не становятся фактическим MQTT-состоянием.

### 11.3. Pending fields

Отдельно отслеживаются:

```text
Power
Mode
Speed
SetTemperature
Blinds
```

`Blok` использует собственное pending-состояние в `DeviceRuntime`.

Когда приходит новый C0:

- pending field очищается только при совпадении фактического и desired значения;
- неподлежащее изменению поле синхронизируется из C0 в cached C3;
- старый C0 не перезаписывает ещё не подтверждённое desired field;
- для Blinds сохраняются актуальные соседние additional-function bits.

## 12. Изменение одного C3-поля

Безопасные функции:

```text
SetRequestPower
SetRequestMode
SetRequestFanSpeed
SetRequestTemperature
SetRequestBlinds
```

Каждая функция:

1. убеждается, что кадр является C3;
2. меняет только своё поле;
3. сохраняет соседние независимые биты;
4. пересчитывает checksum.

Примеры:

- Power изменяет только bit 7 byte 6;
- Mode заменяет mode bits byte 6 и сохраняет Power;
- SetTemp изменяет byte 8;
- Blinds изменяет bit 2 byte 9 и сохраняет остальные known function bits.

## 13. Desired revision и конкурентные MQTT-команды

Каждое изменение desired field увеличивает:

```text
desiredRevision
```

Перед отправкой драйвер берёт immutable snapshot:

```text
frame + revision
```

Если во время физической отправки MQTT изменил cached C3:

- отправленный snapshot завершается как отдельная операция;
- `FinishSetFrameSend()` обнаруживает новую revision;
- устройство снова ставится в C3 queue;
- более новая команда не теряется.

Несколько команд, обработанных до отправки, объединяются в один полный C3.

Например:

```text
Power=1
Mode=Heat
Speed=Medium
SetTemp=24
```

могут быть отправлены одним C3, содержащим все четыре desired field.

## 14. Порядок транзакций

Приоритет:

1. подтверждающий C0;
2. CC/CD;
3. C3;
4. обычный round-robin C0.

Чтобы команды одного проблемного устройства не остановили polling остальных адресов, действует ограничение:

```text
не более 4 приоритетных операций подряд
```

После такого burst обязательно выполняется один обычный round-robin C0.

Внутренние очереди C3, CC/CD и confirmation дедуплицируются по адресу: один адрес не может иметь несколько одинаковых queue entries одновременно.

## 15. Подтверждение C3

Последовательность:

```text
MQTT command
    |
    v
изменение cached C3 + pending field
    |
    v
C3
    |
    v
C0 confirmation
```

C3 response:

- проверяется по framing, адресу, master ID, checksum и command;
- может отметить успешную доставку запроса;
- не копируется в factual state;
- не публикуется как новое значение.

Подтверждающий C0 ставится даже после C3 timeout, потому что фанкойл мог принять команду, а ответ мог потеряться.

Если C0 возвращает старое значение:

- desired field остаётся pending;
- C3 ставится повторно;
- затем снова выполняется C0.

## 16. Лимит повторов

Для одной desired revision:

```text
maximum C3 attempts = 3
maximum CC/CD attempts = 3
```

После трёх неподтверждённых попыток:

- автоматические повторы прекращаются;
- pending desire сохраняется;
- runtime выставляет `setRetryExhausted` или `blockRetryExhausted`;
- обычный C0 polling продолжается;
- другие адреса не голодают;
- устройство не объявляется offline только из-за неподтверждённой write-команды;
- фактическое MQTT-состояние не заменяется desired значением.

Новая команда создаёт новую revision и получает новый лимит из трёх попыток.

Это особенно важно для SetTemp в Auto: оборудование может ответить на C3, но не применить новую уставку. Драйвер выполняет до трёх попыток и ждёт C0, но не публикует неподтверждённую температуру и не создаёт бесконечный цикл.

## 17. Read failure и write failure

### 17.1. Ошибка C0

Timeout, I/O error или invalid C0:

- увеличивает read failure counters;
- устанавливает `online=false`;
- сохраняет диагностический `lastError`;
- приводит к MQTT `Alarm=2`, `Status=7`;
- не останавливает round-robin;
- адрес будет опрошен снова.

### 17.2. Ошибка C3/CC/CD

Ошибка write-транзакции:

- увеличивает соответствующий write failure counter;
- не объявляет устройство offline сама по себе;
- после неё всё равно выполняется C0 confirmation;
- доступность определяется чтением C0.

Таким образом, отвечающий на C0 фанкойл не становится offline только потому, что write-команда не подтвердилась.

## 18. Входящие MQTT-команды

Подписка:

```text
/devices/+/controls/+/on1
```

Полный topic:

```text
/devices/Fan-<bus>_<address>/controls/<Control>/on1
```

Команда должна быть non-retained и содержать одно целое значение.

Команды другой шины игнорируются соответствующим процессом.

### 18.1. Mapping

| Control | MQTT payload | MDV |
|---|---:|---|
| `Power` | `0`, `1` | Power bit |
| `Mode` | `0` | Cool |
| `Mode` | `1` | Heat |
| `Mode` | `2` | Dry |
| `Mode` | `3` | Fan |
| `Mode` | `4` | Auto |
| `Speed` | `1` | Low |
| `Speed` | `2` | Medium |
| `Speed` | `3` | High |
| `Speed` | `4` | Auto |
| `SetTemp` | `16..32` | C3 byte 8 |
| `Blinds` | `0`, `1` | C3 byte 9 bit 2 |
| `Blok` | `0`, `1` | CD unlock / CC lock |

Отклоняются:

- retained-команды;
- base state topic без `/on1`;
- неизвестный device format;
- неизвестный control;
- нецелый payload;
- payload вне диапазона;
- адрес, не входящий в конфигурацию процесса;
- команда до первого корректного C0.

## 19. Входящая MQTT-очередь

Network callback не изменяет `DeviceContext`.

Он только кладёт `MqttMessage` во входящую очередь. Команда применяется позже в основном driver thread.

Лимиты:

```text
MaximumPendingMessages = 512
MaximumPendingBytes = 256 KiB
```

Ключ объединения:

```text
полный MQTT topic
```

Поведение `BoundedLatestQueue`:

- новая команда той же темы удаляет старую и становится последней;
- разные control topics сохраняются независимо;
- payload больше полного byte budget отклоняется;
- при превышении count или bytes удаляются самые старые элементы;
- очередь остаётся FIFO для оставшихся последних значений.

Основной цикл обрабатывает не более 64 MQTT-сообщений перед одной физической транзакцией. MQTT flood не должен остановить C0/C3.

## 20. Фактические MQTT-состояния

Device:

```text
Fan-<bus>_<address>
```

Retained base topics:

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

Правила:

- публикация выполняется только после `PollRead` или `ConfirmRead`;
- успешный результат должен содержать корректный C0;
- C3/CC/CD не публикуются как state;
- публикации retained;
- без `force` публикуются только изменения;
- значения никогда не публикуются в `/on1`.

### 20.1. Неизвестные значения C0

Если C0 не содержит распознаваемый Mode или Speed, соответствующий topic не обновляется.

Если SetTemp вне `16..32`, `SetTemp` не обновляется.

Нельзя подставлять desired C3 для заполнения отсутствующего фактического значения.

### 20.2. Очистка недоступной T1

Если T1 становится `0xFF`, драйвер публикует в retained `Temp` пустой payload:

```text
/devices/Fan-B_A/controls/Temp = ""
```

Это удаляет устаревшее retained numeric value у broker и сообщает текущим подписчикам, что температура недоступна.

Повторный одинаковый unavailable state не создаёт бесконечный поток пустых публикаций. Forced snapshot публикует очистку повторно.

## 21. Alarm, AlarmCode и Status

### 21.1. Alarm

```text
0 = нет E-error
1 = присутствует E-error
2 = нет связи / offline
```

### 21.2. AlarmCode

Используется первый установленный error bit:

```text
errorsE0E7 bit 0..7 -> AlarmCode 1..8
errorsE8EF bit 0..7 -> AlarmCode 9..16
```

Текущая реализация разбирает protections и communication error bytes, но не включает их в вычисление `AlarmCode` и не публикует их отдельными controls.

### 21.3. Status

```text
0 = Power off
1 = Cool
2 = Heat
3 = Dry
4 = Fan
5 = Auto
6 = Alarm
7 = Offline
```

При наличии E-error Status имеет значение `6` независимо от Power/Mode.

`Blok` публикуется отдельно и не заменяет рабочий Status.

## 22. Offline и восстановление

При неуспешном C0 сразу публикуются:

```text
Alarm = 2
Status = 7
```

Повторный timeout не повторяет те же retained значения.

После следующего корректного C0 публикуются только необходимые изменения:

```text
Alarm = 0 или 1
AlarmCode = актуальный код
Status = актуальный режим
```

Остальные factual values остаются последними подтверждёнными C0 до получения новых значений.

## 23. Системное MQTT-устройство

Имя:

```text
sist-<bus>
```

Controls:

```text
/devices/sist-<bus>/controls/Serial
/devices/sist-<bus>/controls/Error
/devices/sist-<bus>/controls/GanGetID
```

### 23.1. Serial

При запуске:

```text
Порт открыт
```

При штатном завершении основного процесса:

```text
Порт закрыт
```

`mdvwb-offline` повторно публикует закрытый порт после `ExecStopPost`.

### 23.2. Error

- успешная транзакция очищает Error;
- I/O error и invalid response публикуют текст ошибки;
- timeout отдельного фанкойла не перезаписывает системный Error;
- обычный timeout выражается через `Alarm=2` и `Status=7` самого устройства.

### 23.3. GanGetID

Публикуется только с `--publish-poll-address`.

## 24. MQTT metadata Wiren Board

На подключении или переподключении публикуются retained metadata:

```text
/devices/Fan-<bus>_<address>/meta/...
/devices/Fan-<bus>_<address>/controls/<Control>/meta/...
/devices/sist-<bus>/meta/...
```

Имена:

```text
Кондиционер <bus>-<address>
Статус опроса кондиционеров <bus>
```

Текущие control metadata имеют:

```text
readonly = 1
```

Это означает, что стандартный интерфейс Wiren Board рассматривает их как factual controls. Управление остаётся отдельным контрактом `/on1`.

Metadata controls:

```text
Alarm
AlarmCode
Blinds
Blok
Mode
Power
SetTemp
Speed
Status
Temp
Serial
Error
GanGetID
```

## 25. MQTT reconnect и доставка

`MosquittoMqttClient` использует асинхронный libmosquitto loop.

При reconnect он:

- повторяет все сохранённые subscriptions;
- отправляет накопленные retained publications;
- сохраняет только последнее retained значение каждой темы.

Правила disconnect:

- retained state можно поставить в reconnect queue;
- несколько retained publications одной темы объединяются;
- non-retained event или command не ставится в reconnect queue;
- недоставленная non-retained публикация возвращает ошибочный delivery status;
- replay старой команды после reconnect запрещён.

Текущие MQTT публикации используют QoS 0.

## 26. Начальный snapshot и reconnect snapshot

При переходе MQTT в connected:

1. публикуется metadata всех настроенных устройств;
2. планируется forced state snapshot;
3. delay выбирается как максимум из:

```text
3 секунды
полный round-robin + 1 секунда
```

После delay для каждого адреса вызывается forced publication.

Если устройство к этому моменту не имеет корректного C0, snapshot публикует offline state.

Та же процедура повторяется после MQTT reconnect.

## 27. `mdvwb-offline` и systemd

`mdvwb@.service` содержит:

```text
ExecStart=/usr/local/lib/mdvwb/mdvwb-run
ExecStopPost=-/usr/local/lib/mdvwb/mdvwb-run --publish-offline
Restart=on-failure
RestartSec=5
```

`ExecStopPost` выполняется и после неожиданного завершения.

`mdvwb-run --publish-offline` читает тот же generated environment file и запускает:

```text
/usr/local/bin/mdvwb-offline
```

Offline publisher:

- проверяет уникальные адреса `0..63`;
- проверяет bus `1..999`;
- подключается к MQTT до 5 секунд;
- публикует retained offline state;
- ждёт доставки до 5 секунд;
- после пустой pending queue выдерживает settle delay 500 ms;
- завершает работу с ошибкой, если публикации не доставлены.

Префикс `-` у `ExecStopPost` не позволяет ошибке offline publisher изменить systemd-результат остановки основного сервиса.

## 28. Exit codes

### 28.1. `MDVWB`

Общие:

```text
0 = успех
1 = self-test failure
2 = configuration/usage error
3 = startup/runtime error
```

Специальные режимы также используют:

```text
4 = discovery отменён или manual test не получил первый C0
5 = manual command не подтверждена в лимите slots
```

### 28.2. `mdvwb-offline`

```text
0 = публикации доставлены
2 = configuration/usage error
3 = runtime/MQTT delivery error
```

## 29. Профильные CTest

### 29.1. `mdv_protocol_self_test`

Команда:

```text
MDVWB --self-test
```

Покрывает протокол, cache, serial, polling, command confirmation, MQTT state, metadata, discovery и CLI.

### 29.2. `mdvwb_offline_publisher_test`

Команда:

```text
mdvwb-offline --self-test
```

Проверяет arguments и retained offline publications.

### 29.3. `mdvwb_mqtt_delivery_test`

Проверяет:

- disconnected retained queue;
- запрет очереди non-retained event;
- coalescing retained по topic;
- factual Mode/Speed/SetTemp/T1 только из C0;
- очистку stale Temp при T1=`0xFF`.

### 29.4. `mdvwb_driver_fairness_test`

Проверяет:

- ровно три C3 attempt одной revision;
- ровно три CC attempt одной revision;
- прекращение автоматических повторов;
- сохранение pending desire;
- продолжение polling других адресов;
- priority burst не более четырёх;
- новый desired revision получает новый retry budget.

## 30. Проверки при изменении протокола

Обязательные проверки:

- request size 16;
- response size 32;
- wire request `0xFE + frame`;
- checksum request bytes `1..14`;
- checksum response bytes `1..30`;
- command complement byte;
- expected master ID;
- expected address;
- fixed collector;
- `0x55` внутри payload;
- ресинхронизация после повреждённого кадра;
- ровно один исходящий Mode;
- ровно одна исходящая Speed;
- Auto + physical decoding;
- T1=`0xFF`.

## 31. Проверки при изменении команд

Обязательные проверки:

- запрет write до первого C0;
- изменение только нужного byte/bit;
- сохранение соседних полей;
- обновление checksum;
- pending field;
- desired revision;
- immutable send snapshot;
- C0 после C3/CC/CD;
- старый C0 не удаляет desired field;
- максимум три attempt;
- новый command сбрасывает retry budget;
- polling остальных адресов продолжается;
- C3 не публикуется как fact.

## 32. Проверки при изменении MQTT

Обязательные проверки:

- command suffix `/on1`;
- command non-retained;
- factual state на base topic;
- factual state retained;
- публикация только после C0;
- mapping Mode/Speed;
- SetTemp `16..32`;
- очистка Temp empty payload;
- Alarm/Status offline и recovery;
- `Blok` отдельно от Status;
- system timeout не засоряет Error;
- callback только ставит сообщение в очередь;
- очередь ограничена count и bytes;
- same-topic latest wins;
- reconnect хранит retained, но не non-retained;
- metadata остаётся согласованной со state topics.

## 33. Инварианты, которые нельзя нарушать

- Один процесс `MDVWB` владеет одним serial port.
- Управляются только адреса `0..63`.
- Broadcast `0xFF` не используется.
- Все транзакции одной шины используют один pacer.
- Период не может быть меньше `150 ms`.
- Power независим от Mode.
- Исходящий C3 содержит ровно один Mode.
- Исходящий C3 содержит ровно одну Speed.
- Только корректный C0 обновляет actual state.
- C3/CC/CD response не является factual state.
- Write-команда запрещена до первого корректного C0.
- Confirmation выполняется C0.
- Для одной desired revision выполняется не более трёх write attempts.
- Исчерпание retry не останавливает polling.
- Командные MQTT topics заканчиваются `/on1`.
- Retained-команды отклоняются.
- Factual state публикуется retained на base topics.
- T1=`0xFF` очищает stale retained Temp.
- Timeout C0 публикует `Alarm=2`, `Status=7`.
- Write failure сам по себе не делает отвечающее устройство offline.
- `Blok` не меняет вычисление Status.
- MQTT callback не изменяет serial/device state.
- Non-retained MQTT events не воспроизводятся после reconnect.
- После остановки сервиса offline publisher обновляет retained availability.

## 34. Связанные подсистемы

Управление несколькими шинами, JSON-конфигурация, systemd synchronization, dashboard, background upload, migration и scheduler реализованы вне `src/driver`.

Их основные каталоги:

```text
src/manager/
src/scheduler/
www/mdvwb/
www/fancoils/
deploy/
```

Подробные контракты этих подсистем описываются в следующих разделах документационного аудита и должны проверяться по соответствующим исходникам и тестам, а не выводиться из поведения драйвера.
