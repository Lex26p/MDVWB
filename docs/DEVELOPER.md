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

Подробные контракты dashboard, scheduler и deployment описываются в следующих шагах документационного аудита. Контракты manager, `buses.json`, systemd synchronization, discovery и legacy migration описаны ниже.

## 35. Назначение `mdvwb-manager`

`mdvwb-manager` — отдельный C++20-процесс, который владеет конфигурационными и lifecycle-операциями проекта.

Менеджер:

- читает и валидирует `/etc/mdvwb/buses.json`;
- создаёт производные `/etc/default/mdvwb-<bus>`;
- запускает, останавливает и перезапускает `mdvwb@<bus>.service`;
- публикует конфигурацию и статусы по MQTT;
- принимает новую конфигурацию с optimistic concurrency;
- защищает ссылки dashboard и scheduler при изменении шин;
- очищает obsolete retained MQTT topics;
- запускает discovery отдельным процессом `MDVWB --discover`;
- мигрирует legacy environment-файлы в первый `buses.json`;
- обслуживает dashboard и schedules MQTT API, которые подробно описаны в следующих разделах документации.

Менеджер не открывает RS-485-порты сам. Физический serial port открывает только соответствующий процесс `MDVWB`.

Production service:

```text
mdvwb-manager.service
```

Точка входа:

```text
mdvwb-manager mqtt /etc/mdvwb/buses.json
```

## 36. Карта исходников manager

| Файл | Ответственность |
|---|---|
| `src/manager/mdv_buses_config.cpp`, `.h` | Строгий parser, validator и canonical serializer `buses.json` |
| `src/manager/mdvwb_service_sync.cpp`, `.h` | Производные environment-файлы, service plan, atomic write и systemd |
| `src/manager/mdvwb_manager_cli.cpp`, `.h` | CLI-команды и коды завершения |
| `src/manager/mdvwb_manager_main.cpp` | Минимальная точка входа executable |
| `src/manager/mdvwb_manager_mqtt.cpp`, `.h` | Long-lived MQTT endpoint, revision checks, transactional apply и bus lifecycle |
| `src/manager/mdvwb_discovery_runner.cpp`, `.h` | Безопасный запуск `MDVWB --discover`, timeout и разбор результата |
| `src/manager/mdvwb_migration.cpp`, `.h` | Строгая миграция legacy `/etc/default/mdvwb*` без выполнения shell-кода |
| `src/manager/mdv_dashboard_config.cpp`, `.h` | Dashboard schema и reference checks |
| `src/manager/mdv_schedules_config.cpp`, `.h` | Schedules schema и reference checks |
| `src/manager/mdvwb_dashboard_upload.cpp`, `.h` | Chunked upload подложки dashboard |

## 37. CLI `mdvwb-manager`

Поддерживаемые команды:

```text
mdvwb-manager validate [buses.json]
mdvwb-manager show [buses.json]
mdvwb-manager summary [buses.json]
mdvwb-manager plan [buses.json]
mdvwb-manager apply [buses.json]
mdvwb-manager mqtt [buses.json]
mdvwb-manager migrate-defaults [buses.json]
```

### 37.1. Разрешение пути

Путь выбирается в порядке:

1. явный второй аргумент;
2. `MDVWB_BUSES_CONFIG`;
3. `/etc/mdvwb/buses.json`.

### 37.2. `validate`

Только загружает и проверяет JSON.

Успешный результат:

```text
CONFIG_OK buses=<count> enabled=<count>
```

Файлы и сервисы не изменяются.

### 37.3. `show`

Печатает canonical JSON:

- buses отсортированы по `id`;
- addresses отсортированы;
- присутствует `revision`;
- форматирование стабильно.

### 37.4. `summary`

Печатает компактные строки:

```text
version=1
buses=<count>
enabled=<count>
bus=<id> enabled=<true|false> port=<path> addresses=<csv>
```

Текущая реализация `summary` не печатает `revision`.

### 37.5. `plan`

Строит и печатает service synchronization plan, но не применяет его.

Возможные действия:

```text
WRITE_CONFIG
REMOVE_CONFIG
ENABLE_START
ENABLE_RESTART
DISABLE_STOP
ENSURE_ENABLED_STARTED
NO_CHANGES
```

`plan` читает environment template и текущие `/etc/default/mdvwb-*`, поэтому требует доступ к этим файлам, но не требует root только из-за самой команды.

### 37.6. `apply`

Загружает `buses.json`, строит plan и применяет его.

`apply`:

- требует root;
- не увеличивает `revision`;
- не перезаписывает сам `buses.json`;
- синхронизирует только runtime environment-файлы и systemd.

Изменение `revision` и транзакционная запись `buses.json` выполняются через MQTT save endpoint менеджера.

### 37.7. `mqtt`

Запускает long-lived manager daemon и требует root.

### 37.8. `migrate-defaults`

Строго читает legacy `/etc/default/mdvwb*`, создаёт canonical `BusesConfig` и только после полного успеха атомарно записывает целевой `buses.json`.

Команда требует root.

### 37.9. Коды завершения

```text
0 = успех
1 = manager/runtime/root error
2 = usage или configuration error
```

Диагностические префиксы:

```text
CONFIG_ERROR:
MANAGER_ERROR:
USAGE_ERROR:
```

## 38. Canonical `buses.json`

Production path:

```text
/etc/mdvwb/buses.json
```

Schema version:

```text
1
```

Пример:

```json
{
  "version": 1,
  "revision": 4,
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

Root fields:

```text
version
revision
buses
```

Bus fields:

```text
id
enabled
port
addresses
```

Неизвестные fields отклоняются.

## 39. Проверка `buses.json`

### 39.1. Root

- root должен быть object;
- `version` обязателен и равен `1`;
- `revision` необязателен при чтении и по умолчанию равен `0`;
- `revision` — integer `0..2147483647`;
- `buses` обязателен и является array;
- duplicate JSON object keys отклоняются;
- floating-point и exponent numbers не поддерживаются;
- trailing content после root отклоняется.

Canonical serializer всегда записывает `revision`.

### 39.2. Bus ID

```text
1..999
```

ID уникальны.

### 39.3. Port

Port:

- является string;
- начинается с `/dev/`;
- содержит хотя бы один символ после `/dev/`;
- уникален между всеми buses;
- допускает только alphanumeric и:

```text
/ _ - . + :
```

Примеры допустимых путей:

```text
/dev/ttyRS485-1
/dev/ttyUSB0
/dev/serial/by-id/usb-adapter_1
```

### 39.4. Addresses

Каждый address:

```text
0..63
```

Addresses уникальны внутри bus.

Canonical serializer сортирует их по возрастанию.

### 39.5. Enabled bus

Для:

```json
"enabled": true
```

список addresses не может быть пустым.

Disabled bus может иметь:

```json
"addresses": []
```

### 39.6. Canonical order

Serializer сортирует buses по `id`.

Порядок bus objects во входном JSON не сохраняется.

## 40. Revision `buses.json`

`revision` реализует optimistic concurrency для браузеров и других MQTT-клиентов.

Клиент должен:

1. получить retained `/mdvwb/config`;
2. изменить локальную копию;
3. сохранить исходную полученную `revision`;
4. опубликовать изменённый JSON в `/mdvwb/config/set`.

Manager сравнивает:

```text
submitted.revision == current.revision
```

При совпадении:

```text
saved.revision = current.revision + 1
```

При конфликте:

- сохранение отклоняется;
- systemd не изменяется;
- текущая конфигурация не перезаписывается;
- сначала публикуется non-retained result;
- затем повторно публикуется текущий retained `/mdvwb/config`;
- публикуется актуальный ready status.

Если существующий `buses.json` отсутствует или повреждён, manager допускает восстановление только submitted revision `0`.

При достижении `2147483647` новое сохранение отклоняется.

## 41. MQTT topics manager

### 41.1. Общая конфигурация

```text
/mdvwb/config
/mdvwb/config/set
/mdvwb/config/result
/mdvwb/status
```

| Topic | Retain | Назначение |
|---|---:|---|
| `/mdvwb/config` | Да | Текущий canonical `buses.json` |
| `/mdvwb/config/set` | Нет | Новая конфигурация |
| `/mdvwb/config/result` | Нет | Результат save/apply |
| `/mdvwb/status` | Да | Общий status manager |

Успешный manager status:

```json
{
  "state": "ready",
  "buses": 2,
  "enabled": 1
}
```

Ошибка:

```json
{
  "state": "error",
  "message": "..."
}
```

Configuration result содержит:

```json
{
  "success": true,
  "saved": true,
  "message": "Configuration saved and applied",
  "buses": 2,
  "enabled": 1,
  "actions": 4
}
```

`success` и `saved` различаются:

- `success=true, saved=true` — файл и runtime применены;
- `success=false, saved=false` — новая конфигурация не сохранена;
- `success=false, saved=true` — submitted recovery target сохранён, но systemd degraded и rollback неполон.

### 41.2. Payload limits

```text
buses.json set payload = максимум 65536 bytes
dashboard set payload = максимум 1048576 bytes
schedules set payload = максимум 1048576 bytes
```

Retained save-команды отклоняются.

## 42. Manager incoming queue

Network callback только разбирает topic и кладёт операцию в bounded queue.

Лимиты:

```text
MaximumPendingCommands = 128
MaximumPendingBytes = 8 MiB
```

Queue key:

- bus `start`, `stop` и `restart` одной шины используют один общий key;
- `finish` и `cancel` одного upload используют один общий key;
- остальные операции объединяются по полному MQTT topic.

Следствие:

- последовательные lifecycle-команды одной шины сохраняют только последнюю;
- status request, discovery и config set имеют отдельные keys;
- разные bus ID независимы;
- при превышении budget удаляются самые старые операции;
- один payload больше 8 MiB не принимается в queue.

Физические и systemd-операции выполняются не из MQTT callback, а из `ProcessOne()` manager loop.

## 43. Защита dashboard и schedules references

Перед сохранением новой bus configuration manager загружает:

```text
dashboard.json
schedules.json
```

и проверяет ссылки на:

```text
bus
address
panel
```

Manager сравнивает issues текущей и submitted bus configuration.

Отклоняется изменение, которое создаёт новые broken references.

Уже существующие issues не мешают исправлять другие части `buses.json`, если submitted configuration не добавляет новые проблемы.

Таким образом нельзя молча:

- удалить bus, который используется dashboard или schedule;
- удалить address, на который есть ссылка;
- изменить конфигурацию так, чтобы существующая корректная ссылка стала недействительной.

После успешного изменения шин manager повторно публикует dashboard и schedules status.

## 44. Производный environment-файл

Для каждой configured bus целевой путь:

```text
/etc/default/mdvwb-<bus>
```

Основа:

```text
/usr/local/lib/mdvwb/mdvwb.env
```

Manager заменяет или добавляет:

```text
MDVWB_BUS
MDVWB_PORT
MDVWB_ADDRESSES
```

Другие переменные template сохраняются, например:

```text
MDVWB_MASTER_ID
MDVWB_PERIOD_MS
MDVWB_RESPONSE_TIMEOUT_MS
MDVWB_MQTT_HOST
MDVWB_MQTT_PORT
```

В начало generated file добавляется marker:

```text
# Managed by mdvwb-manager from buses.json.
```

Этот marker используется при поиске obsolete runtime-файлов.

Environment values экранируются для двойных кавычек, backslash, `$` и backtick.

Generated file имеет permissions:

```text
owner read/write
group read
others none
```

то есть эквивалент `0640`.

## 45. Atomic text write

Manager пишет текстовый файл через sibling temporary file:

```text
<target>.tmp
```

Последовательность:

1. создать parent directories;
2. записать полный content;
3. установить `0640`;
4. rename temporary в target;
5. при платформе, не заменяющей existing target через rename, удалить target и повторить rename;
6. при ошибке удалить temporary и вернуть ошибку.

Для MQTT config transaction дополнительно используется:

```text
buses.json.pending
buses.json.previous
```

`pending` является staged validated configuration. `previous` применяется только как временный backup при платформенном commit fallback.

## 46. Service synchronization plan

Manager сравнивает desired config с существующим `/etc/default/mdvwb-<id>`.

### 46.1. Enabled bus, файл отсутствует

```text
WRITE_CONFIG
ENABLE_START
```

Systemd:

```text
systemctl enable --now mdvwb@<id>.service
```

### 46.2. Enabled bus, файл изменился

```text
WRITE_CONFIG
ENABLE_RESTART
```

Systemd:

```text
systemctl enable mdvwb@<id>.service
systemctl restart mdvwb@<id>.service
```

### 46.3. Enabled bus, файл не изменился

```text
ENSURE_ENABLED_STARTED
```

Systemd:

```text
systemctl enable --now mdvwb@<id>.service
```

Это восстанавливает вручную остановленный или disabled service.

### 46.4. Disabled bus

При необходимости сначала:

```text
WRITE_CONFIG
```

затем:

```text
DISABLE_STOP
systemctl disable --now mdvwb@<id>.service
```

Runtime environment сохраняется, чтобы bus можно было снова включить.

### 46.5. Bus удалён из `buses.json`

Только generated file с managed marker считается автоматически удаляемым.

Plan:

```text
DISABLE_STOP
REMOVE_CONFIG
```

Сначала service отключается и останавливается, затем удаляется `/etc/default/mdvwb-<id>`.

Посторонние файлы без managed marker не удаляются как obsolete.

## 47. Transactional save и apply

MQTT save `buses.json` является транзакцией, насколько это возможно для файлов и systemd.

Последовательность успешного save:

1. загрузить previous `buses.json`;
2. проверить submitted revision;
3. проверить schema;
4. проверить dashboard/schedules references;
5. увеличить revision;
6. построить service plan;
7. сохранить snapshot previous file;
8. записать canonical JSON в `buses.json.pending`;
9. применить generated files и systemd plan;
10. commit staged file в `buses.json`;
11. опубликовать retained config и statuses;
12. очистить obsolete retained device topics;
13. опубликовать non-retained successful result.

Source `buses.json` не заменяется до успешного завершения service plan.

## 48. Rollback при ошибке apply

Если environment или systemd action завершилась ошибкой, manager строит rollback plan из previous configuration.

### 48.1. Rollback успешен

- previous generated files и service states восстанавливаются;
- previous `buses.json` остаётся либо восстанавливается;
- `.pending` удаляется;
- retained current config повторно публикуется;
- result:

```text
success=false
saved=false
```

### 48.2. Rollback systemd неполон, submitted file удалось сохранить

Submitted canonical config сохраняется как recovery target.

Manager публикует retained error status и result:

```text
success=false
saved=true
```

Это означает: файл содержит желаемое состояние, но фактический systemd может ему не соответствовать.

### 48.3. Не удалось сохранить ни previous, ни submitted target

Manager публикует explicit degraded error с деталями apply и rollback.

Оператор должен сверить:

```text
/etc/mdvwb/buses.json
/etc/default/mdvwb-*
systemctl status mdvwb@*.service
```

## 49. Bus lifecycle MQTT API

Command topics:

```text
/mdvwb/buses/<id>/start
/mdvwb/buses/<id>/stop
/mdvwb/buses/<id>/restart
/mdvwb/buses/<id>/status/get
```

Status/result:

```text
/mdvwb/buses/<id>/status
/mdvwb/buses/<id>/result
```

Command payload текущим handler не интерпретируется. Значение может быть пустым; команда определяется topic.

Retained bus commands отклоняются.

### 49.1. Start

Разрешён только для configured bus с:

```json
"enabled": true
```

Выполняет:

```text
systemctl start mdvwb@<id>.service
```

### 49.2. Stop

Разрешён для configured bus независимо от `enabled`.

Выполняет:

```text
systemctl stop mdvwb@<id>.service
```

Stop не изменяет `buses.json` и не отключает autostart.

### 49.3. Restart

Разрешён только для configured enabled bus.

Выполняет:

```text
systemctl restart mdvwb@<id>.service
```

### 49.4. Status get

Не выполняет lifecycle command, а повторно публикует retained status.

### 49.5. Bus status payload

```json
{
  "id": 1,
  "configured": true,
  "enabled": true,
  "service": "active",
  "autostart": true,
  "port": "/dev/ttyRS485-1",
  "addresses": [1, 2, 3]
}
```

Значения `service` и `autostart` читаются через:

```text
systemctl is-active --quiet
systemctl is-enabled --quiet
```

### 49.6. Bus result payload

```json
{
  "success": true,
  "bus": 1,
  "command": "restart",
  "message": "Bus service restarted"
}
```

Result non-retained.

## 50. Очистка obsolete retained topics

После успешного сохранения bus configuration manager сравнивает previous и current config.

Для каждого удалённого address очищаются retained topics:

```text
/devices/Fan-<bus>_<address>/meta/...
/devices/Fan-<bus>_<address>/controls/<Control>
/devices/Fan-<bus>_<address>/controls/<Control>/on1
/devices/Fan-<bus>_<address>/controls/<Control>/meta/...
```

Controls:

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
```

Очистка выполняется retained publication с пустым payload.

Для полностью удалённой bus дополнительно очищаются:

```text
/devices/sist-<bus>/...
/mdvwb/buses/<bus>/status
/mdvwb/buses/<bus>/discovery/status
/mdvwb/buses/<bus>/discovery/result
```

Очистка происходит только после successful save/apply, а не при rejected revision или rollback.

## 51. Discovery MQTT API

Запуск:

```text
/mdvwb/buses/<id>/discovery/start
```

Публикации:

```text
/mdvwb/buses/<id>/discovery/status
/mdvwb/buses/<id>/discovery/result
/mdvwb/buses/<id>/result
```

Retained discovery command отклоняется.

Bus должен существовать в current `buses.json`.

## 52. Подготовка discovery

Перед запуском manager:

1. публикует retained discovery status `running`;
2. проверяет `systemctl is-active`;
3. если service active — выполняет `systemctl stop`;
4. повторно публикует bus status;
5. запускает отдельный worker.

Manager не выполняет automatic restart после discovery.

Bus остаётся остановленной до явного:

```text
/mdvwb/buses/<id>/start
```

или повторного `apply`/изменения конфигурации, которое обеспечит enabled/start state.

Найденные addresses не записываются в `buses.json` автоматически.

## 53. Параллельность discovery

Manager хранит tasks в map:

```text
busId -> DiscoveryTask
```

Правила:

- для одного bus ID одновременно существует не более одного task;
- повторный discovery той же bus отклоняется;
- discovery разных bus ID могут выполняться параллельно;
- завершение одной bus не забирает result другой;
- worker completion обрабатывается основным manager loop;
- destructor manager join-ит все оставшиеся workers.

После старта manager ждёт до `100 ms`, чтобы быстрый test runner мог вернуть результат inline. Реальный serial scan обычно продолжается в background.

## 54. Native discovery runner

Manager запускает:

```text
/usr/local/bin/MDVWB \
  --discover \
  --port <bus.port> \
  --master-id 0 \
  --period-ms 150 \
  --response-timeout-ms 130
```

Native runner поддерживается только на Linux.

Лимиты по умолчанию:

```text
overall timeout = 45000 ms
maximum captured stdout+stderr = 256 KiB
```

При timeout:

1. отправляется `SIGTERM`;
2. даётся grace period до 1 секунды;
3. затем применяется `SIGKILL`;
4. child обязательно reap-ится.

При превышении output limit процесс также завершается.

## 55. Разбор discovery output

Runner ищет строки:

```text
FOUND_ADDRESSES=<csv>
```

Используется последнее найденное значение.

Допустимы:

```text
FOUND_ADDRESSES=
FOUND_ADDRESSES=0
FOUND_ADDRESSES=1,5,63
```

Отклоняются:

- отсутствие строки;
- пустой item;
- нецелое значение;
- address вне `0..63`;
- duplicate address.

Результат сортируется по возрастанию.

Exit code процесса должен быть `0`.

## 56. Discovery status и result

Running status:

```json
{
  "bus": 1,
  "state": "running",
  "port": "/dev/ttyRS485-1",
  "message": "Discovery is running"
}
```

Completed status:

```json
{
  "bus": 1,
  "state": "completed",
  "port": "/dev/ttyRS485-1",
  "message": "Discovery completed",
  "found": 3
}
```

Error status:

```json
{
  "bus": 1,
  "state": "error",
  "port": "/dev/ttyRS485-1",
  "message": "..."
}
```

Discovery status retained.

Result:

```json
{
  "success": true,
  "bus": 1,
  "addresses": [1, 5, 63],
  "message": "Discovery completed"
}
```

Discovery result также retained, чтобы браузер после reload видел последний итог.

При старте manager для каждой configured bus публикуется status:

```text
idle
```

## 57. Strict legacy migration

Migration рассматривает только имена:

```text
mdvwb
mdvwb-<decimal bus id>
```

Примеры игнорируемых файлов:

```text
mdvwb-backup
mdvwb.disabled
other-service
```

Malformed candidate вроде:

```text
mdvwb-
mdvwb-01
```

не игнорируется молча, а приводит к ошибке.

Candidates:

- должны быть regular files;
- сортируются по filename для детерминированной диагностики;
- читаются как текст;
- никогда не выполняются через shell.

Если candidate отсутствуют, migration завершается ошибкой:

```text
no legacy MDVWB bus configurations were found
```

## 58. Legacy assignment parser

Допустимы:

```text
пустые строки
# comment
NAME=VALUE
NAME="VALUE"
NAME='VALUE'
```

Синтаксис assignment name:

```text
[A-Za-z_][A-Za-z0-9_]*
```

Parser проверяет синтаксис всех непустых non-comment строк.

Обрабатываются только keys с prefix:

```text
MDVWB_
```

Другие корректные assignments игнорируются.

Не выполняются:

- shell expansion;
- variable substitution;
- command substitution;
- sourcing;
- escape interpretation как в shell.

Quote снимается только при совпадающей первой и последней `'` или `"`. Незакрытая или неожиданная внутренняя quote отклоняется.

Duplicate `MDVWB_*` assignment отклоняется с номером первой строки.

## 59. Обязательные legacy fields

Каждый candidate требует:

```text
MDVWB_PORT
MDVWB_ADDRESSES
```

### 59.1. `mdvwb-<id>`

Bus ID берётся из filename.

`MDVWB_BUS` может отсутствовать.

Если он присутствует, значение обязано совпадать с filename.

### 59.2. Unsuffixed `mdvwb`

Обязательно содержит:

```text
MDVWB_BUS
```

### 59.3. ID

Диапазон:

```text
1..999
```

Filename использует canonical decimal form без leading zeros.

### 59.4. Addresses

- CSV;
- допускается полностью пустой список для disabled legacy service;
- item не может быть пустым;
- address `0..63`;
- duplicates запрещены;
- результат сортируется.

## 60. Ambiguous migration

Migration отклоняет:

- два файла, определяющих один bus ID;
- несовпадение filename и `MDVWB_BUS`;
- duplicate ports;
- invalid `/dev/...` path;
- invalid bus ID;
- malformed assignment;
- missing required assignment;
- duplicate MDVWB key;
- invalid/duplicate address;
- enabled service с пустым address list.

Проверка IDs, ports, collisions и addresses выполняется до systemd queries.

## 61. Определение enabled при migration

После полного разбора sources manager запрашивает:

```text
systemctl is-active --quiet mdvwb@<id>.service
systemctl is-enabled --quiet mdvwb@<id>.service
```

Migrated bus считается enabled, если:

```text
active OR enabled
```

После этого выполняется повторная полная validation.

Поэтому empty address list разрешён только когда service одновременно inactive и disabled.

## 62. Запись результата migration

`migrate-defaults` вызывает:

```text
MigrateLegacyDefaults()
SerializeBusesConfig()
WriteTextFileAtomically()
```

Target file не открывается на запись до завершения parsing, ambiguity checks, schema validation и systemd status queries.

При ошибке migration существующий target не изменяется.

Начальная revision:

```text
0
```

Сам CLI strict. Поведение установщиков при ошибке migration рассматривается отдельно в документации deployment, потому что installer может применять собственный fallback.

## 63. Профильные manager CTest

### 63.1. `mdvwb_buses_config_test`

Проверяет strict JSON schema, unknown fields, ranges, duplicates и canonical serializer.

### 63.2. `mdvwb_manager_cli_test`

Проверяет CLI-команды, path resolution, root restrictions и output.

### 63.3. `mdvwb_service_sync_test`

Проверяет generated environment, action plan, atomic writes и systemctl commands.

### 63.4. `mdvwb_manager_mqtt_test`

Проверяет manager topics, save/apply, bus lifecycle, statuses, cleanup и reference behavior.

### 63.5. `mdvwb_manager_revision_test`

Проверяет stale revision rejection и порядок:

```text
result -> current retained configuration
```

### 63.6. `mdvwb_manager_transaction_test`

Проверяет successful rollback и explicit degraded state при rollback failure.

### 63.7. `mdvwb_discovery_runner_test`

Проверяет запуск subprocess, parsing, timeout и output limits.

### 63.8. `mdvwb_discovery_async_test`

Проверяет:

- same-bus collision rejection;
- independent different-bus workers;
- корректную привязку completion к bus ID.

### 63.9. `mdvwb_migration_test`

Проверяет valid migration и набор malformed/ambiguous legacy sources.

### 63.10. `mdvwb_mqtt_queue_test`

Проверяет bounded manager queue, latest-by-key и eviction.

## 64. Проверки при изменении `buses.json`

Обязательно проверить:

- schema version `1`;
- revision;
- unknown fields;
- duplicate JSON keys;
- bus ID `1..999`;
- unique ID;
- safe `/dev/` path;
- unique port;
- address `0..63`;
- unique address;
- enabled bus не пуст;
- canonical sorting;
- reference protection;
- stale revision;
- revision increment;
- payload limit;
- retained command rejection.

## 65. Проверки при изменении systemd synchronization

Обязательно проверить:

- generated marker;
- template preservation;
- замену BUS/PORT/ADDRESSES;
- permissions `0640`;
- new enabled bus;
- changed enabled bus;
- unchanged enabled bus;
- disabled bus;
- removed managed bus;
- unmanaged obsolete file не удаляется;
- apply failure;
- successful rollback;
- incomplete rollback;
- `.pending` cleanup;
- saved flag соответствует фактическому состоянию файла.

## 66. Проверки при изменении discovery

Обязательно проверить:

- bus существует;
- retained command rejected;
- active service остановлен;
- inactive service не получает лишний stop;
- service автоматически не запускается;
- config не изменяется;
- addresses не применяются;
- same bus serialized;
- different buses independent;
- master ID `0`;
- period `150 ms`;
- timeout `130 ms`;
- process timeout `45 s`;
- output limit `256 KiB`;
- `FOUND_ADDRESSES`;
- status/result retain;
- completion относится к правильному bus ID.

## 67. Проверки при изменении migration

Обязательно проверить:

- sources не выполняются как shell;
- deterministic candidate order;
- regular file;
- exact candidate names;
- canonical filename ID;
- required PORT/ADDRESSES;
- unsuffixed BUS;
- filename/BUS match;
- duplicate assignment;
- malformed line;
- quote errors;
- address range и duplicates;
- duplicate bus sources;
- duplicate ports;
- pre-systemd validation;
- active/enabled mapping;
- final enabled-empty rejection;
- target unchanged on failure.
