# MDVWB

Компактный C++20-драйвер индивидуального управления фанкойлами MDV через RS-485 и MQTT на контроллере Wiren Board.

## Поддерживаемые функции

Драйвер опрашивает устройства индивидуально и управляет следующими параметрами:

- `Power`: `0` — выключено, `1` — включено;
- `Mode`: `0` — охлаждение, `1` — обогрев, `2` — осушение, `3` — вентиляция, `4` — авто;
- `Speed`: `1` — низкая, `2` — средняя, `3` — высокая, `4` — авто;
- `SetTemp`: от `16` до `32` °C;
- `Blinds`: `0` — выключено, `1` — включено;
- `Blok`: `0` — разблокировать, `1` — заблокировать.

Также публикуются `Temp`, `Alarm`, `AlarmCode` и общий `Status`. Значение `Blok` публикуется отдельно и не меняет общий `Status`.

## MQTT-топики

Команды принимаются из топиков:

```text
/devices/Fan-1_1/controls/Power/on1
/devices/Fan-1_1/controls/Mode/on1
/devices/Fan-1_1/controls/Speed/on1
/devices/Fan-1_1/controls/SetTemp/on1
/devices/Fan-1_1/controls/Blinds/on1
/devices/Fan-1_1/controls/Blok/on1
```

Фактическое состояние публикуется только при изменении:

```text
/devices/Fan-1_1/controls/Power/on
/devices/Fan-1_1/controls/Mode/on
/devices/Fan-1_1/controls/Speed/on
/devices/Fan-1_1/controls/SetTemp/on
/devices/Fan-1_1/controls/Temp/on
/devices/Fan-1_1/controls/Blinds/on
/devices/Fan-1_1/controls/Blok/on
/devices/Fan-1_1/controls/Alarm/on
/devices/Fan-1_1/controls/AlarmCode/on
/devices/Fan-1_1/controls/Status/on
```

Системные топики:

```text
/devices/sist-1/controls/Serial
/devices/sist-1/controls/Error
/devices/sist-1/controls/GanGetID
```

`GanGetID` выключен по умолчанию и включается параметром `--publish-poll-address`.

## Безопасность обмена

- Скорость порта: `4800 8N1`.
- Минимальный период между началами транзакций: `150 мс`.
- По умолчанию ответ ожидается до `130 мс`.
- На линии одновременно выполняется только одна транзакция.
- Кадр установки формируется только после первого корректного `C0`.
- MQTT-команда меняет только нужное поле уже подготовленного кадра.
- После `C3`, `CC` или `CD` выполняется отдельный подтверждающий `C0`.
- Старый ответ после установки не затирает ожидаемую команду.
- Широковещательное управление не используется.

## Сборка в Visual Studio

```powershell
cmake --preset x64-debug
cmake --build "out/build/x64-debug"
.\out\build\x64-debug\MDVWB.exe --self-test
```

Версия:

```powershell
.\out\build\x64-debug\MDVWB.exe --version
```

## Безопасная проверка через USB–COM

Сначала выполняется только чтение одного адреса. Замените `COM4` и адрес `1` на фактические значения:

```powershell
.\out\build\x64-debug\MDVWB.exe --addresses 1 --port COM4 --bus 1 --read-only
```

В этом режиме разрешены только запросы `C0`. MQTT и команды записи отключены. Остановка — `Ctrl+C`.

После стабильного чтения одна команда проверяется отдельным запуском:

```powershell
.\out\build\x64-debug\MDVWB.exe --addresses 1 --port COM4 --bus 1 --test-command SetTemp=24
```

Другие допустимые варианты:

```text
Power=0 или Power=1
Mode=0..4
Speed=1..4
SetTemp=16..32
Blinds=0 или Blinds=1
Blok=0 или Blok=1
```

За один запуск разрешена только одна команда и только один адрес. Драйвер сначала читает фактическое состояние, затем отправляет команду и подтверждает результат отдельным `C0`.

## Рабочий запуск

Совместимый формат:

```text
MDVWB 1,2,3 /dev/ttyRS485-1 1
```

Именованный формат:

```text
MDVWB --addresses 1,2,3 --port /dev/ttyRS485-1 --bus 1 --mqtt-host 127.0.0.1
```

Полный список параметров:

```text
MDVWB --help
```

## Установка на Wiren Board

На контроллере должны быть установлены компилятор, CMake и `libmosquitto-dev`. Для сборки и установки используется:

```bash
sudo ./deploy/install_wirenboard.sh
```

После установки отредактируйте:

```text
/etc/default/mdvwb
```

Затем запустите сервис:

```bash
sudo systemctl enable --now mdvwb.service
sudo systemctl status mdvwb.service
```

## Инициализация MQTT после запуска

Первый корректный C0 публикует полный набор значений устройства. Дополнительно
драйвер один раз повторяет полный снимок после запуска MQTT и после каждого
переподключения. Задержка равна не менее 3 секунд и не меньше одного полного
цикла опроса плюс 1 секунда. Это исключает сохранение старых значений
виртуального устройства, если wb-rules создаёт контролы позже первого ответа.


## Управление и диагностика сервиса из веб-интерфейса Wiren Board

Файл `deploy/mdvwb-service-control.js` создаёт виртуальное устройство `MDVWB-Service-1`. Оно содержит кнопки запуска, остановки, перезапуска, обновления статуса и обновления диагностики.

Диагностический запрос показывает:

- текущее состояние `mdvwb.service`;
- состояние автозапуска systemd;
- версию фактически установленного `/usr/local/bin/MDVWB`;
- последние 12 записей журнала сервиса;
- результат последней команды.

Скрипт можно вручную скопировать или отредактировать через раздел правил веб-интерфейса Wiren Board. Он не принимает произвольные shell-команды и управляет только `mdvwb.service`.
