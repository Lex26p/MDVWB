# MDVWB

Компактный C++20-драйвер индивидуального управления фанкойлами MDV через
RS-485 и MQTT на Wiren Board.

## Архитектура

- один процесс `MDVWB` обслуживает один последовательный порт;
- каждая активная шина запускается отдельным экземпляром `mdvwb@N.service`;
- общий `mdvwb-manager.service` читает `/etc/mdvwb/buses.json`, синхронизирует
  процессы и предоставляет MQTT API для статической страницы;
- страница устанавливается в `/mnt/data/www/mdvwb/` и открывается по адресу
  `http://<WB-address>/mdvwb/`;
- драйвер сам публикует retained-метаданные `Fan-<bus>_<address>` и `sist-<bus>`,
  поэтому отдельный wb-rules-скрипт с `ArrID` больше не нужен.

## Конфигурация шин

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
      "port": "/dev/serial/by-id/mdv-bus-2",
      "addresses": [5, 18]
    }
  ]
}
```

Поддерживается произвольное количество шин `1..999`, индивидуальный путь
`/dev/...` и адреса фанкойлов `0..63`.

## MQTT

Фактические значения публикуются retained в основные топики:

```text
/devices/Fan-1_1/controls/Power
/devices/Fan-1_1/controls/Mode
/devices/Fan-1_1/controls/Speed
/devices/Fan-1_1/controls/SetTemp
/devices/Fan-1_1/controls/Temp
/devices/Fan-1_1/controls/Blinds
/devices/Fan-1_1/controls/Blok
/devices/Fan-1_1/controls/Alarm
/devices/Fan-1_1/controls/AlarmCode
/devices/Fan-1_1/controls/Status
```

Команды принимаются только из безопасных `/on1`-топиков с `retain=false`:

```text
/devices/Fan-1_1/controls/Power/on1
/devices/Fan-1_1/controls/Mode/on1
/devices/Fan-1_1/controls/Speed/on1
/devices/Fan-1_1/controls/SetTemp/on1
/devices/Fan-1_1/controls/Blinds/on1
/devices/Fan-1_1/controls/Blok/on1
```

Менеджер использует `/mdvwb/config`, `/mdvwb/config/set`,
`/mdvwb/buses/<id>/...` для веб-настройки, управления и поиска.

## Сборка и тесты

```powershell
cmake --preset x64-debug
cmake --build "out/build/x64-debug"
ctest --test-dir "out/build/x64-debug" -C Debug --output-on-failure
```

## Установка на Wiren Board

GitHub Actions создаёт artifact `MDVWB-arm64-offline`. После копирования
архива на контроллер:

```bash
cd /root
rm -rf MDVWB-arm64
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
chmod +x offline-install.sh
./offline-install.sh
```

Установщик сохраняет существующий `/etc/mdvwb/buses.json`. При первой
миграции он преобразует текущие `/etc/default/mdvwb-N`, отключает старый
wb-rules-скрипт с `ArrID`, очищает старые retained `Fan-*` и запускает новую
схему.

## Диагностика

```bash
systemctl status mdvwb-manager.service --no-pager
mdvwb-manager summary /etc/mdvwb/buses.json
systemctl status 'mdvwb@*.service' --no-pager
journalctl -u mdvwb-manager.service -n 50 --no-pager
journalctl -u mdvwb@1.service -n 50 --no-pager
```
