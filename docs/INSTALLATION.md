# MDVWB: установка, настройка и эксплуатация

> Документ описывает установку и эксплуатацию MDVWB 1.2.0 на Wiren Board ARM64.  
> Основной целевой контроллер: Debian 11 Bullseye, ARM64, systemd и Mosquitto.  
> Команды Linux выполняются от `root`, если явно не указано обратное.

## 1. Назначение документа

Здесь описаны:

- подготовка Wiren Board;
- рекомендуемая offline-установка;
- установка из исходников;
- ручная установка;
- обновление без потери конфигурации;
- настройка одной или нескольких RS-485-шин;
- проверка после установки;
- полный справочник команд `MDVWB`;
- полный справочник команд `mdvwb-manager`;
- управление systemd-сервисами;
- MQTT-проверки;
- discovery;
- аппаратный тест команд;
- резервное копирование;
- восстановление и удаление;
- диагностика типовых проблем.

Инструкция по работе с браузерной страницей и обычному управлению фанкойлами находится в:

```text
docs/WEB_AND_FANCOILS.md
```

## 2. Что устанавливается

После полной установки используются следующие файлы:

```text
/usr/local/bin/MDVWB
/usr/local/bin/mdvwb-manager
/usr/local/lib/mdvwb/mdvwb-run
/usr/local/lib/mdvwb/mdvwb.env

/etc/mdvwb/buses.json
/etc/default/mdvwb-manager
/etc/default/mdvwb-<bus>

/etc/systemd/system/mdvwb@.service
/etc/systemd/system/mdvwb-manager.service

/var/www/mdvwb/index.html
/var/www/mdvwb/app.js
/var/www/mdvwb/model.js
/var/www/mdvwb/mqtt-client.js
/var/www/mdvwb/styles.css
```

Стандартный адрес веб-интерфейса:

```text
http://<адрес-WB>/mdvwb/
```

Для IPv6:

```text
http://[IPv6-адрес]/mdvwb/
```

Завершающий `/` рекомендуется использовать всегда.

## 3. Модель процессов

Для каждой активной RS-485-шины запускается отдельный экземпляр:

```text
mdvwb@1.service
mdvwb@2.service
mdvwb@3.service
...
```

Отдельно работает:

```text
mdvwb-manager.service
```

Менеджер читает `/etc/mdvwb/buses.json`, создаёт runtime-файлы и управляет экземплярами `mdvwb@N.service`.

Один процесс `MDVWB` владеет одним serial port. Два процесса не должны использовать один и тот же `/dev/...`.

## 4. Требования

### 4.1. Аппаратные

- Wiren Board ARM64;
- RS-485-порт контроллера или USB–RS-485;
- подключённая шина MDV XYE;
- корректная линия A/B и общий GND при необходимости;
- уникальные адреса фанкойлов на каждой физической шине.

### 4.2. Системные

Для runtime:

```text
systemd
Mosquitto
libmosquitto.so.1
```

Для online-сборки:

```text
build-essential
cmake
libmosquitto-dev
```

### 4.3. Сеть

Для offline-установки интернет на Wiren Board не требуется.

Для работы браузерной страницы должны быть доступны:

- HTTP/HTTPS интерфейс Wiren Board;
- MQTT WebSocket endpoint `/mqtt`;
- обычный MQTT broker для драйвера и менеджера.

## 5. Предварительная проверка контроллера

### 5.1. Архитектура

```bash
dpkg --print-architecture
```

Ожидается:

```text
arm64
```

Дополнительно:

```bash
uname -m
```

Ожидается:

```text
aarch64
```

### 5.2. Версия ОС

```bash
cat /etc/os-release
```

### 5.3. Mosquitto

```bash
systemctl status mosquitto.service --no-pager
```

Проверка runtime-библиотеки:

```bash
ldconfig -p | grep 'libmosquitto\.so\.1'
```

### 5.4. Последовательные порты

```bash
ls -l /dev/ttyRS485-* /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
```

Для USB-адаптера:

```bash
dmesg | tail -n 50
```

Более стабильный путь устройства можно искать так:

```bash
find /dev/serial/by-id -maxdepth 1 -type l -ls 2>/dev/null
```

Если `/dev/serial/by-id/...` начинается с `/dev/` и содержит только допустимые символы, его можно использовать в `buses.json`.

## 6. Рекомендуемый вариант: offline-установка

Offline artifact собирается GitHub Actions workflow:

```text
Build ARM64 Offline Package
```

Название artifact:

```text
MDVWB-arm64-offline
```

Внутри:

```text
MDVWB-arm64-offline.tar.gz
MDVWB-arm64-offline.tar.gz.sha256
```

### 6.1. Проверка архива на компьютере

PowerShell:

```powershell
Get-FileHash "C:\Users\pereverworkki\Downloads\MDVWB-arm64-offline.tar.gz" -Algorithm SHA256
```

Сравните результат с содержимым:

```text
MDVWB-arm64-offline.tar.gz.sha256
```

### 6.2. Копирование

Скопируйте оба файла на контроллер, например в:

```text
/root
```

Способ копирования не важен: веб-интерфейс, SCP, SFTP или USB-накопитель.

### 6.3. Проверка checksum на Wiren Board

```bash
cd /root
sha256sum -c MDVWB-arm64-offline.tar.gz.sha256
```

Ожидается:

```text
MDVWB-arm64-offline.tar.gz: OK
```

### 6.4. Распаковка и установка

```bash
cd /root
rm -rf MDVWB-arm64
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
chmod +x offline-install.sh
./offline-install.sh
```

Установщик:

1. Проверяет запуск от root.
2. Проверяет архитектуру `arm64`.
3. Проверяет `libmosquitto.so.1`.
4. Проверяет полноту пакета.
5. Проверяет `SHA256SUMS` внутри пакета.
6. Останавливает менеджер на время обновления.
7. Устанавливает бинарники и systemd units.
8. Устанавливает веб-файлы в `/var/www/mdvwb`.
9. Сохраняет существующий непустой `buses.json`.
10. При первой установке мигрирует старые `/etc/default/mdvwb-N`.
11. Отключает устаревшие фиксированные сервисы.
12. Безопасно отключает старый wb-rules-файл с `ArrID`, если он найден.
13. Выполняет self-test и проверку конфигурации.
14. Применяет конфигурацию шин.
15. Запускает `mdvwb-manager.service`.

## 7. Настраиваемый web root

Правильный путь на протестированном контроллере:

```text
/var/www/mdvwb
```

Установщик использует:

```text
MDVWB_WWW_ROOT
```

Например:

```bash
MDVWB_WWW_ROOT=/var/www ./offline-install.sh
```

Без необходимости эту переменную менять не следует. Стандартную маршрутизацию веб-интерфейса Wiren Board изменять не требуется.

## 8. Online-установка из исходников на Wiren Board

Online-вариант требует интернет и установку пакетов.

В корне репозитория:

```bash
chmod +x deploy/install_wirenboard.sh
deploy/install_wirenboard.sh
```

Скрипт выполняет:

```text
apt-get update
apt-get install build-essential cmake libmosquitto-dev
CMake Release build
CTest
установку бинарников
установку systemd
установку web
миграцию/создание buses.json
apply
запуск manager
```

Изменение каталога сборки:

```bash
MDVWB_BUILD_DIR=/root/mdvwb-build deploy/install_wirenboard.sh
```

Явное указание web root:

```bash
MDVWB_WWW_ROOT=/var/www deploy/install_wirenboard.sh
```

## 9. Ручная сборка из исходников

### 9.1. Debug

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

### 9.2. Release с обязательным Mosquitto

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMDVWB_REQUIRE_MOSQUITTO=ON
cmake --build build-release
ctest --test-dir build-release --output-on-failure
```

### 9.3. Проверка бинарников

```bash
./build-release/MDVWB --version
./build-release/MDVWB --self-test
./build-release/mdvwb-manager validate config/buses.example.json
```

## 10. Ручная установка готовых бинарников

Ручная установка используется только при необходимости диагностики или разработки.

Создание каталогов:

```bash
install -d -m 0755 \
  /usr/local/bin \
  /usr/local/lib/mdvwb \
  /etc/mdvwb \
  /etc/default \
  /etc/systemd/system \
  /var/www/mdvwb
```

Установка бинарников:

```bash
install -m 0755 MDVWB /usr/local/bin/MDVWB
install -m 0755 mdvwb-manager /usr/local/bin/mdvwb-manager
```

Установка runtime:

```bash
install -m 0755 deploy/mdvwb-run /usr/local/lib/mdvwb/mdvwb-run
install -m 0640 deploy/mdvwb.env /usr/local/lib/mdvwb/mdvwb.env
```

Установка systemd:

```bash
install -m 0644 deploy/mdvwb@.service /etc/systemd/system/mdvwb@.service
install -m 0644 deploy/mdvwb-manager.service /etc/systemd/system/mdvwb-manager.service
```

Установка manager environment при его отсутствии:

```bash
test -e /etc/default/mdvwb-manager || \
  install -m 0640 deploy/mdvwb-manager.env /etc/default/mdvwb-manager
```

Установка страницы:

```bash
cp -a www/mdvwb/. /var/www/mdvwb/
find /var/www/mdvwb -type d -exec chmod 0755 {} +
find /var/www/mdvwb -type f -exec chmod 0644 {} +
```

Установка примера конфигурации только при отсутствии реальной:

```bash
test -s /etc/mdvwb/buses.json || \
  install -m 0640 deploy/buses.example.json /etc/mdvwb/buses.json
```

Применение:

```bash
systemctl daemon-reload
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager apply /etc/mdvwb/buses.json
systemctl enable --now mdvwb-manager.service
```

## 11. Обновление установленной версии

Рекомендуемый способ — запустить новый `offline-install.sh`.

Существующий непустой файл:

```text
/etc/mdvwb/buses.json
```

не должен перезаписываться.

Перед обновлением рекомендуется создать резервную копию:

```bash
mkdir -p /root/mdvwb-backup
cp -a /etc/mdvwb/buses.json /root/mdvwb-backup/
cp -a /etc/default/mdvwb-manager /root/mdvwb-backup/ 2>/dev/null || true
cp -a /etc/default/mdvwb-* /root/mdvwb-backup/ 2>/dev/null || true
```

После обновления:

```bash
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
systemctl status mdvwb-manager.service --no-pager
```

## 12. Откат бинарников

Если заранее сохранена резервная копия:

```bash
systemctl stop mdvwb-manager.service
systemctl stop 'mdvwb@*.service'
```

Верните бинарники:

```bash
install -m 0755 /root/mdvwb-backup/MDVWB /usr/local/bin/MDVWB
install -m 0755 /root/mdvwb-backup/mdvwb-manager /usr/local/bin/mdvwb-manager
```

Верните systemd units и runtime-файлы при необходимости, затем:

```bash
systemctl daemon-reload
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager apply /etc/mdvwb/buses.json
systemctl restart mdvwb-manager.service
```

## 13. Конфигурация `/etc/mdvwb/buses.json`

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
      "enabled": true,
      "port": "/dev/ttyUSB0",
      "addresses": [1, 5, 18]
    },
    {
      "id": 3,
      "enabled": false,
      "port": "/dev/ttyUSB1",
      "addresses": []
    }
  ]
}
```

### 13.1. Поля

| Поле | Назначение |
|---|---|
| `version` | Версия схемы, сейчас только `1` |
| `id` | Номер шины `1..999` |
| `enabled` | Должен ли процесс шины работать |
| `port` | Индивидуальный путь `/dev/...` |
| `addresses` | Адреса фанкойлов `0..63` |

### 13.2. Ограничения

- ID шин не повторяются.
- Порты не повторяются.
- Адреса внутри одной шины не повторяются.
- Активная шина содержит минимум один адрес.
- Отключённая шина может иметь пустой список.
- Неизвестные поля запрещены.
- Путь начинается с `/dev/`.
- Адреса и шины при нормализации сортируются.

## 14. Ручное редактирование конфигурации

Создайте резервную копию:

```bash
cp -a /etc/mdvwb/buses.json /etc/mdvwb/buses.json.backup
```

Отредактируйте файл:

```bash
nano /etc/mdvwb/buses.json
```

Или редактором Wiren Board, если он доступен.

Проверьте:

```bash
mdvwb-manager validate /etc/mdvwb/buses.json
```

Посмотрите план:

```bash
mdvwb-manager plan /etc/mdvwb/buses.json
```

Примените:

```bash
mdvwb-manager apply /etc/mdvwb/buses.json
```

Проверьте:

```bash
mdvwb-manager summary /etc/mdvwb/buses.json
systemctl status mdvwb-manager.service --no-pager
```

При ошибке верните резервную копию:

```bash
cp -a /etc/mdvwb/buses.json.backup /etc/mdvwb/buses.json
mdvwb-manager apply /etc/mdvwb/buses.json
```

## 15. Настройка одной шины

```json
{
  "version": 1,
  "buses": [
    {
      "id": 1,
      "enabled": true,
      "port": "/dev/ttyRS485-1",
      "addresses": [1, 2, 3]
    }
  ]
}
```

Применение:

```bash
mdvwb-manager validate /etc/mdvwb/buses.json
mdvwb-manager plan /etc/mdvwb/buses.json
mdvwb-manager apply /etc/mdvwb/buses.json
```

## 16. Настройка нескольких шин

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
      "enabled": true,
      "port": "/dev/ttyRS485-2",
      "addresses": [1, 2]
    },
    {
      "id": 4,
      "enabled": true,
      "port": "/dev/ttyUSB0",
      "addresses": [5, 18]
    }
  ]
}
```

После `apply` ожидаются:

```text
mdvwb@1.service
mdvwb@2.service
mdvwb@4.service
```

Номер `3` использовать необязательно. ID шины — логический идентификатор, а не порядковый номер в массиве.

## 17. Отключение шины без удаления

Измените:

```json
"enabled": false
```

Адреса можно оставить или очистить.

Примените:

```bash
mdvwb-manager apply /etc/mdvwb/buses.json
```

Сервис будет остановлен и отключён, но шина останется в конфигурации и веб-интерфейсе.

## 18. Удаление шины

Удалите объект шины из массива `buses`, затем:

```bash
mdvwb-manager validate /etc/mdvwb/buses.json
mdvwb-manager plan /etc/mdvwb/buses.json
mdvwb-manager apply /etc/mdvwb/buses.json
```

Менеджер должен:

- остановить соответствующий сервис;
- отключить его;
- удалить только свой runtime environment-файл;
- очистить устаревшие retained-топики этой сущности.

## 19. Конфигурация MQTT менеджера

Файл:

```text
/etc/default/mdvwb-manager
```

Основные переменные:

```bash
MDVWB_MQTT_HOST="127.0.0.1"
MDVWB_MQTT_PORT="1883"
MDVWB_MQTT_USER=""
MDVWB_MQTT_PASSWORD=""
MDVWB_MQTT_KEEPALIVE="60"
MDVWB_MQTT_RECONNECT="1"
MDVWB_MQTT_RECONNECT_MAX="10"

MDVWB_BUSES_CONFIG="/etc/mdvwb/buses.json"
MDVWB_DEFAULT_DIR="/etc/default"
MDVWB_ENV_TEMPLATE="/usr/local/lib/mdvwb/mdvwb.env"
MDVWB_BINARY="/usr/local/bin/MDVWB"
```

После изменения:

```bash
systemctl restart mdvwb-manager.service
```

## 20. Проверка после установки

### 20.1. Версия

```bash
/usr/local/bin/MDVWB --version
```

Ожидается:

```text
MDVWB 1.2.0
```

### 20.2. Self-test

```bash
/usr/local/bin/MDVWB --self-test
```

Ожидается строка с окончанием:

```text
self-test: OK
```

### 20.3. Конфигурация

```bash
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
```

Пример:

```text
CONFIG_OK buses=2 enabled=2
```

### 20.4. Сводка

```bash
/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json
```

### 20.5. Менеджер

```bash
systemctl status mdvwb-manager.service --no-pager
```

### 20.6. Все экземпляры

```bash
systemctl list-units 'mdvwb@*.service' --all --no-pager
```

### 20.7. Конкретная шина

```bash
systemctl status mdvwb@1.service --no-pager
```

### 20.8. Web-файлы

```bash
ls -la /var/www/mdvwb
```

### 20.9. HTTP

```bash
curl -I http://127.0.0.1/mdvwb/
```

### 20.10. MQTT состояния

```bash
mosquitto_sub -v -C 10 -W 20 -t '/devices/Fan-1_1/#'
```

## 21. Полный справочник `MDVWB`

### 21.1. Помощь

```bash
MDVWB --help
```

### 21.2. Версия

```bash
MDVWB --version
```

### 21.3. Self-test

```bash
MDVWB --self-test
```

### 21.4. Обычный запуск

```bash
MDVWB --addresses 1,2,3 --port /dev/ttyRS485-1 --bus 1
```

Старый формат:

```bash
MDVWB 1,2,3 /dev/ttyRS485-1 1
```

### 21.5. Обязательные параметры обычного запуска

```text
--addresses LIST
--port PATH
--bus NUMBER
```

### 21.6. Master ID и тайминги

```text
--master-id NUMBER
--period-ms NUMBER
--response-timeout-ms NUMBER
```

Пример:

```bash
MDVWB \
  --addresses 1,2,3 \
  --port /dev/ttyRS485-1 \
  --bus 1 \
  --master-id 0 \
  --period-ms 150 \
  --response-timeout-ms 130
```

Ограничения:

- `master-id`: `0..63`;
- `period-ms`: не менее `150`;
- timeout должен быть меньше периода.

### 21.7. MQTT-параметры

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

Пример:

```bash
MDVWB \
  --addresses 1 \
  --port /dev/ttyUSB0 \
  --bus 2 \
  --mqtt-host 127.0.0.1 \
  --mqtt-port 1883 \
  --mqtt-client-id mdvwb-2
```

### 21.8. Read-only

```bash
MDVWB \
  --addresses 1,2,3 \
  --port /dev/ttyRS485-1 \
  --bus 1 \
  --read-only
```

### 21.9. Discovery

```bash
MDVWB \
  --port /dev/ttyRS485-1 \
  --bus 1 \
  --discover
```

Discovery нельзя объединять с:

```text
--addresses
--read-only
--test-command
```

### 21.10. Аппаратный тест команды

```bash
MDVWB \
  --addresses 1 \
  --port /dev/ttyRS485-1 \
  --bus 1 \
  --test-command Power=1
```

Должен быть указан ровно один адрес.

Поддерживаются:

```text
Power=0
Power=1

Mode=0
Mode=1
Mode=2
Mode=3
Mode=4

Speed=1
Speed=2
Speed=3
Speed=4

SetTemp=16..32

Blinds=0
Blinds=1

Blok=0
Blok=1
```

### 21.11. Публикация текущего опрашиваемого адреса

```text
--publish-poll-address
```

Пример:

```bash
MDVWB \
  --addresses 1,2,3 \
  --port /dev/ttyRS485-1 \
  --bus 1 \
  --publish-poll-address
```

Опция нужна только для диагностики и увеличивает MQTT-трафик.

## 22. Полный справочник `mdvwb-manager`

### 22.1. Помощь

```bash
mdvwb-manager --help
```

Допустимы также:

```bash
mdvwb-manager help
mdvwb-manager -h
```

### 22.2. Проверка конфигурации

```bash
mdvwb-manager validate
```

Или:

```bash
mdvwb-manager validate /etc/mdvwb/buses.json
```

### 22.3. Канонический JSON

```bash
mdvwb-manager show /etc/mdvwb/buses.json
```

### 22.4. Сводка

```bash
mdvwb-manager summary /etc/mdvwb/buses.json
```

### 22.5. Предварительный план

```bash
mdvwb-manager plan /etc/mdvwb/buses.json
```

`plan` не должен изменять файлы или systemd.

### 22.6. Применение

```bash
mdvwb-manager apply /etc/mdvwb/buses.json
```

Требует root.

### 22.7. MQTT-демон

```bash
mdvwb-manager mqtt /etc/mdvwb/buses.json
```

Обычно вручную не запускается: эту команду выполняет `mdvwb-manager.service`.

### 22.8. Миграция старых конфигов

```bash
mdvwb-manager migrate-defaults /etc/mdvwb/buses.json
```

Команда читает старые `/etc/default/mdvwb-N` и создаёт новый JSON.

Не запускайте её поверх существующего рабочего JSON без резервной копии.

### 22.9. Разрешение пути

Если путь не указан, используется:

1. `MDVWB_BUSES_CONFIG`;
2. `/etc/mdvwb/buses.json`.

### 22.10. Коды завершения

```text
0 = успех
1 = runtime/manager error
2 = usage/configuration error
```

## 23. Команды systemd

### 23.1. Менеджер

Статус:

```bash
systemctl status mdvwb-manager.service --no-pager
```

Запуск:

```bash
systemctl start mdvwb-manager.service
```

Остановка:

```bash
systemctl stop mdvwb-manager.service
```

Перезапуск:

```bash
systemctl restart mdvwb-manager.service
```

Автозапуск:

```bash
systemctl enable mdvwb-manager.service
```

Отключение автозапуска:

```bash
systemctl disable mdvwb-manager.service
```

### 23.2. Конкретная шина

```bash
systemctl status mdvwb@1.service --no-pager
systemctl start mdvwb@1.service
systemctl stop mdvwb@1.service
systemctl restart mdvwb@1.service
```

Ручное управление service не изменяет `buses.json`.

После следующего `mdvwb-manager apply` состояние будет снова приведено к конфигурации.

### 23.3. Все шины

```bash
systemctl list-units 'mdvwb@*.service' --all --no-pager
```

Активные процессы:

```bash
systemctl --type=service --state=running | grep 'mdvwb@'
```

### 23.4. Перечитать units

```bash
systemctl daemon-reload
```

Требуется после ручной замены unit-файлов.

## 24. Журналы

### 24.1. Менеджер

Последние записи:

```bash
journalctl -u mdvwb-manager.service -n 100 --no-pager
```

Наблюдение:

```bash
journalctl -u mdvwb-manager.service -f
```

Текущая загрузка:

```bash
journalctl -u mdvwb-manager.service -b --no-pager
```

### 24.2. Шина

```bash
journalctl -u mdvwb@1.service -n 100 --no-pager
```

Наблюдение:

```bash
journalctl -u mdvwb@1.service -f
```

### 24.3. Все компоненты

```bash
journalctl -u mdvwb-manager.service -u 'mdvwb@*.service' -b --no-pager
```

Если glob не поддерживается вашей версией journalctl, укажите units отдельно.

## 25. MQTT API конфигурации

Подписка:

```bash
mosquitto_sub -v \
  -t '/mdvwb/config' \
  -t '/mdvwb/config/result' \
  -t '/mdvwb/status'
```

Отправка конфигурации:

```bash
mosquitto_pub \
  -t '/mdvwb/config/set' \
  -m '{"version":1,"buses":[{"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1,2,3]}]}'
```

Не используйте retain:

```text
-r
```

для командных топиков.

Текущий config публикуется retained:

```bash
mosquitto_sub -v -C 1 -W 10 -t '/mdvwb/config'
```

## 26. MQTT-управление шинами

Запуск:

```bash
mosquitto_pub -t '/mdvwb/buses/1/start' -m '1'
```

Остановка:

```bash
mosquitto_pub -t '/mdvwb/buses/1/stop' -m '1'
```

Перезапуск:

```bash
mosquitto_pub -t '/mdvwb/buses/1/restart' -m '1'
```

Обновить статус:

```bash
mosquitto_pub -t '/mdvwb/buses/1/status/get' -m '1'
```

Подписка:

```bash
mosquitto_sub -v \
  -t '/mdvwb/buses/+/status' \
  -t '/mdvwb/buses/+/result'
```

Start/restart отклоняются для шины с:

```json
"enabled": false
```

## 27. Discovery через MQTT

Подписка:

```bash
mosquitto_sub -v \
  -t '/mdvwb/buses/1/discovery/status' \
  -t '/mdvwb/buses/1/discovery/result'
```

Запуск:

```bash
mosquitto_pub -t '/mdvwb/buses/1/discovery/start' -m '1'
```

Поведение:

- останавливается только `mdvwb@1.service`;
- остальные шины продолжают работать;
- после поиска шина остаётся остановленной;
- найденные адреса не применяются;
- пользователь вручную редактирует config и запускает/применяет шину.

После ручного добавления адресов:

```bash
mdvwb-manager apply /etc/mdvwb/buses.json
```

Или через веб-интерфейс сохраните configuration, затем запустите шину.

## 28. MQTT фанкойлов

Пример устройства:

```text
Fan-1_3
```

### 28.1. Просмотр всех данных

```bash
mosquitto_sub -v -t '/devices/Fan-1_3/#'
```

### 28.2. Фактические состояния

```bash
mosquitto_sub -v \
  -t '/devices/Fan-1_3/controls/Power' \
  -t '/devices/Fan-1_3/controls/Mode' \
  -t '/devices/Fan-1_3/controls/Speed' \
  -t '/devices/Fan-1_3/controls/SetTemp' \
  -t '/devices/Fan-1_3/controls/Temp' \
  -t '/devices/Fan-1_3/controls/Alarm' \
  -t '/devices/Fan-1_3/controls/AlarmCode' \
  -t '/devices/Fan-1_3/controls/Status'
```

### 28.3. Включение

```bash
mosquitto_pub \
  -t '/devices/Fan-1_3/controls/Power/on1' \
  -m '1'
```

### 28.4. Выключение

```bash
mosquitto_pub \
  -t '/devices/Fan-1_3/controls/Power/on1' \
  -m '0'
```

### 28.5. Режим

```bash
mosquitto_pub \
  -t '/devices/Fan-1_3/controls/Mode/on1' \
  -m '0'
```

Значения:

```text
0 = Cool
1 = Heat
2 = Dry
3 = Fan
4 = Auto
```

### 28.6. Скорость

```bash
mosquitto_pub \
  -t '/devices/Fan-1_3/controls/Speed/on1' \
  -m '4'
```

Значения:

```text
1 = Low
2 = Medium
3 = High
4 = Auto
```

### 28.7. Температура

```bash
mosquitto_pub \
  -t '/devices/Fan-1_3/controls/SetTemp/on1' \
  -m '24'
```

Допустимый диапазон драйвера:

```text
16..32
```

### 28.8. Жалюзи

```bash
mosquitto_pub \
  -t '/devices/Fan-1_3/controls/Blinds/on1' \
  -m '1'
```

### 28.9. Блокировка

Заблокировать:

```bash
mosquitto_pub \
  -t '/devices/Fan-1_3/controls/Blok/on1' \
  -m '1'
```

Разблокировать:

```bash
mosquitto_pub \
  -t '/devices/Fan-1_3/controls/Blok/on1' \
  -m '0'
```

Командные сообщения всегда должны быть non-retained.

## 29. Проверка metadata

```bash
mosquitto_sub -v \
  -C 20 \
  -W 15 \
  -t '/devices/Fan-1_1/meta/#' \
  -t '/devices/Fan-1_1/controls/+/meta/#'
```

Metadata должна публиковаться retained.

Проверка системного устройства:

```bash
mosquitto_sub -v -t '/devices/sist-1/#'
```

## 30. Проверка retained

Получить одно уже сохранённое состояние:

```bash
mosquitto_sub -v -C 1 -W 5 \
  -t '/devices/Fan-1_1/controls/Power'
```

Если состояние было опубликовано, оно должно прийти сразу.

Никогда не отправляйте команду так:

```bash
mosquitto_pub -r -t '/devices/Fan-1_1/controls/Power/on1' -m '1'
```

Драйвер намеренно отклоняет retained-команды.

## 31. Проверка реального обмена

Откройте журнал:

```bash
journalctl -u mdvwb@1.service -f
```

Отдельно смотрите состояния:

```bash
mosquitto_sub -v -t '/devices/Fan-1_1/controls/#'
```

Отправьте команду:

```bash
mosquitto_pub \
  -t '/devices/Fan-1_1/controls/SetTemp/on1' \
  -m '23'
```

Нормальное поведение:

1. Команда принимается.
2. Отправляется C3.
3. Выполняется подтверждающий C0.
4. Фактический `SetTemp` может измениться не в самом первом ответе.
5. После подтверждения публикуется новое retained-состояние.

## 32. Проверка offline-состояния

Отключите конкретный фанкойл или укажите отсутствующий адрес только для теста.

Ожидается:

```text
Alarm = 2
Status = 7
```

Timeout одного адреса не должен остановить опрос остальных адресов той же шины.

После восстановления связи должны вернуться реальные значения.

## 33. Резервное копирование

Минимальная резервная копия:

```bash
tar -czf /root/mdvwb-config-backup.tar.gz \
  /etc/mdvwb \
  /etc/default/mdvwb-manager \
  /etc/default/mdvwb-* \
  /var/www/mdvwb \
  2>/dev/null
```

Полная runtime-копия:

```bash
tar -czf /root/mdvwb-runtime-backup.tar.gz \
  /usr/local/bin/MDVWB \
  /usr/local/bin/mdvwb-manager \
  /usr/local/lib/mdvwb \
  /etc/mdvwb \
  /etc/default/mdvwb-manager \
  /etc/default/mdvwb-* \
  /etc/systemd/system/mdvwb@.service \
  /etc/systemd/system/mdvwb-manager.service \
  /var/www/mdvwb \
  2>/dev/null
```

## 34. Восстановление конфигурации

```bash
systemctl stop mdvwb-manager.service
tar -xzf /root/mdvwb-config-backup.tar.gz -C /
mdvwb-manager validate /etc/mdvwb/buses.json
mdvwb-manager apply /etc/mdvwb/buses.json
systemctl restart mdvwb-manager.service
```

## 35. Удаление проекта

Перед удалением сохраните конфигурацию, если она может понадобиться:

```bash
cp -a /etc/mdvwb/buses.json /root/buses.json.backup
```

Остановка:

```bash
systemctl disable --now mdvwb-manager.service
```

Получить ID шин:

```bash
mdvwb-manager summary /etc/mdvwb/buses.json
```

Остановите экземпляры:

```bash
for unit in $(systemctl list-unit-files 'mdvwb@*.service' --no-legend | awk '{print $1}'); do
  systemctl disable --now "$unit" 2>/dev/null || true
done
```

Удаление файлов:

```bash
rm -f /usr/local/bin/MDVWB
rm -f /usr/local/bin/mdvwb-manager
rm -rf /usr/local/lib/mdvwb
rm -f /etc/systemd/system/mdvwb@.service
rm -f /etc/systemd/system/mdvwb-manager.service
rm -rf /var/www/mdvwb
rm -f /etc/default/mdvwb-manager
rm -f /etc/default/mdvwb-*
```

Удалять пользовательскую конфигурацию следует отдельно:

```bash
rm -rf /etc/mdvwb
```

Завершение:

```bash
systemctl daemon-reload
systemctl reset-failed
```

Отключённый установщиком старый wb-rules-файл не восстанавливается автоматически.

## 36. Восстановление старого wb-rules-файла

Проверьте:

```bash
ls -l /etc/wb-rules/*.disabled-mdvwb 2>/dev/null
```

Восстанавливать его следует только после полного отключения MDVWB metadata, иначе появятся дубли устройств.

Пример:

```bash
mv \
  /etc/wb-rules/<имя>.js.disabled-mdvwb \
  /etc/wb-rules/<имя>.js
systemctl restart wb-rules.service
```

## 37. Диагностика: менеджер не запускается

```bash
systemctl status mdvwb-manager.service --no-pager
journalctl -u mdvwb-manager.service -n 200 --no-pager
```

Проверьте config:

```bash
mdvwb-manager validate /etc/mdvwb/buses.json
```

Проверьте environment:

```bash
cat /etc/default/mdvwb-manager
```

Проверьте binary:

```bash
/usr/local/bin/mdvwb-manager --help
ldd /usr/local/bin/mdvwb-manager
```

Проверьте Mosquitto:

```bash
systemctl status mosquitto.service --no-pager
```

## 38. Диагностика: сервис шины не запускается

```bash
systemctl status mdvwb@1.service --no-pager
journalctl -u mdvwb@1.service -n 200 --no-pager
```

Проверьте runtime config:

```bash
cat /etc/default/mdvwb-1
```

Проверьте порт:

```bash
ls -l /dev/ttyRS485-1
```

Проверьте, кто использует порт:

```bash
fuser -v /dev/ttyRS485-1 2>/dev/null
```

Проверьте plan:

```bash
mdvwb-manager plan /etc/mdvwb/buses.json
```

## 39. Диагностика: порт меняется после перезагрузки

USB-путь `/dev/ttyUSB0` может измениться.

Проверьте:

```bash
find /dev/serial/by-id -maxdepth 1 -type l -ls
```

Используйте стабильный `/dev/serial/by-id/...` в `buses.json`, если он соответствует правилам validator.

После изменения:

```bash
mdvwb-manager validate /etc/mdvwb/buses.json
mdvwb-manager apply /etc/mdvwb/buses.json
```

## 40. Диагностика: устройства не появляются в Wiren Board

Проверьте сервис:

```bash
systemctl status mdvwb@1.service --no-pager
```

Проверьте metadata:

```bash
mosquitto_sub -v -C 10 -W 10 \
  -t '/devices/Fan-1_1/meta/#' \
  -t '/devices/Fan-1_1/controls/+/meta/#'
```

Проверьте фактические данные:

```bash
mosquitto_sub -v -C 10 -W 20 \
  -t '/devices/Fan-1_1/controls/#'
```

Убедитесь, что старый `ArrID`-скрипт отключён:

```bash
grep -R 'var[[:space:]]\+ArrID' /etc/wb-rules 2>/dev/null
```

## 41. Диагностика: веб открывает обычную страницу WB

Правильное расположение файлов:

```text
/var/www/mdvwb
```

Проверка:

```bash
ls -la /var/www/mdvwb
```

Должны быть:

```text
index.html
app.js
model.js
mqtt-client.js
styles.css
```

Открывайте:

```text
http://<адрес-WB>/mdvwb/
```

Для IPv6:

```text
http://[IPv6]/mdvwb/
```

Если файлов нет, скопируйте их из распакованного offline-пакета:

```bash
install -d -m 0755 /var/www/mdvwb
cp -a /root/MDVWB-arm64/www/mdvwb/. /var/www/mdvwb/
find /var/www/mdvwb -type d -exec chmod 0755 {} +
find /var/www/mdvwb -type f -exec chmod 0644 {} +
```

После копирования выполните жёсткое обновление страницы браузера.

Штатную конфигурацию веб-сервера Wiren Board менять не требуется.

## 42. Диагностика: веб не подключается к MQTT

Проверьте Mosquitto:

```bash
systemctl status mosquitto.service --no-pager
```

Проверьте обычный MQTT:

```bash
mosquitto_sub -v -t '/mdvwb/status'
```

Проверьте браузерную консоль.

Страница подключается к текущему host и WebSocket path:

```text
/mqtt
```

Если стандартный Wiren Board WebSocket работает для штатного интерфейса, отдельная настройка обычно не требуется.

## 43. Диагностика: config сохраняется, но сервис не запускается

Посмотрите `/mdvwb/config/result`.

Возможен результат:

```text
saved=true
success=false
```

Это означает:

- JSON прошёл проверку;
- файл сохранён;
- ошибка возникла на этапе systemd или runtime.

Проверьте:

```bash
mdvwb-manager plan /etc/mdvwb/buses.json
mdvwb-manager apply /etc/mdvwb/buses.json
journalctl -u mdvwb-manager.service -n 200 --no-pager
```

## 44. Диагностика: команды не выполняются

Проверьте:

1. Topic заканчивается на `/on1`.
2. Сообщение отправлено без retain.
3. Payload содержит одно целое.
4. Шина и адрес совпадают.
5. Устройство уже ответило хотя бы на один C0.
6. Значение находится в допустимом диапазоне.
7. Сервис шины работает.

Подписка на результат обмена:

```bash
journalctl -u mdvwb@1.service -f
```

## 45. Диагностика: после команды сразу видно старое значение

Это может быть нормальным поведением фанкойла.

Драйвер не считает C3-ответ подтверждением. Он выполняет последующий C0.

Подождите несколько циклов опроса и смотрите base state topic:

```bash
mosquitto_sub -v \
  -t '/devices/Fan-1_1/controls/SetTemp'
```

Не проверяйте командный `/on1` как фактическое состояние.

## 46. Диагностика: discovery не запускается

Проверьте:

```bash
systemctl status mdvwb-manager.service --no-pager
journalctl -u mdvwb-manager.service -n 200 --no-pager
```

Проверьте bus ID и port:

```bash
mdvwb-manager summary /etc/mdvwb/buses.json
```

Убедитесь, что port существует:

```bash
ls -l /dev/ttyRS485-1
```

Убедитесь, что другой процесс не держит порт:

```bash
fuser -v /dev/ttyRS485-1
```

Менеджер должен остановить только выбранный `mdvwb@N.service`.

## 47. Диагностика: шина не запустилась после discovery

Это ожидаемое поведение.

После discovery:

- сервис остаётся остановленным;
- результат не применяется автоматически.

Запустить без изменения config:

```bash
systemctl start mdvwb@1.service
```

Или через MQTT:

```bash
mosquitto_pub -t '/mdvwb/buses/1/start' -m '1'
```

Если менялись адреса, сначала сохраните конфигурацию.

## 48. Диагностика: дубли устройств

Проверьте старые wb-rules:

```bash
grep -R 'defineVirtualDevice("Fan-"' /etc/wb-rules 2>/dev/null
```

Проверьте metadata:

```bash
mosquitto_sub -v -t '/devices/Fan-1_1/meta/#'
```

Не должны одновременно работать:

- C++ metadata MDVWB;
- старый `ArrID`/`defineVirtualDevice("Fan-")` script.

## 49. Диагностика retained-топиков

Если удалённое устройство продолжает отображаться, проверьте retained:

```bash
mosquitto_sub -v -C 20 -W 5 -t '/devices/Fan-1_63/#'
```

Если установлен helper:

```bash
mqtt-delete-retained '/devices/Fan-1_63/#'
```

Для массовой очистки действуйте осторожно: удаление retained-топиков стирает metadata и состояние до следующей публикации драйвера.

## 50. Безопасность

Текущая система предполагает доверенную локальную сеть.

Рекомендации:

- ограничить доступ к MQTT;
- использовать MQTT credentials при необходимости;
- не публиковать WebSocket broker в интернет без аутентификации;
- не разрешать непривилегированным пользователям менять `/etc/mdvwb/buses.json`;
- сохранять права `0640` для конфигурации и environment-файлов;
- не помещать произвольные shell-фрагменты в port или другие поля;
- не запускать два драйвера на одном serial port.

## 51. Краткая контрольная проверка

```bash
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json
systemctl status mdvwb-manager.service --no-pager
systemctl list-units 'mdvwb@*.service' --all --no-pager
ls -la /var/www/mdvwb
mosquitto_sub -v -C 1 -W 10 -t '/mdvwb/config'
```

Для первой шины и первого адреса:

```bash
mosquitto_sub -v -C 10 -W 20 -t '/devices/Fan-1_1/#'
```

## 52. Связанные документы

```text
AGENTS.md
docs/DEVELOPER.md
docs/INSTALLATION.md
docs/WEB_AND_FANCOILS.md
README.md
```

`INSTALLATION.md` описывает deployment и эксплуатационные команды. Изменения внутренней архитектуры должны дополнительно отражаться в `DEVELOPER.md` и `AGENTS.md`.
