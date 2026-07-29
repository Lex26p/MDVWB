# MDVWB 1.3.0: установка и жизненный цикл на Wiren Board

> Production-цель: Wiren Board ARM64, Debian 11 Bullseye, systemd, Mosquitto и
> `libmosquitto.so.1`. Команды выполняются от `root` либо через `sudo`.

## 1. Способы установки

Рекомендуемые способы:

1. **Online** — контроллер скачивает готовый ARM64 package из GitHub Release.
2. **Offline** — package переносится на контроллер без доступа в интернет.

Сборка непосредственно на контроллере сохранена только для разработки:

```bash
sudo ./deploy/install-from-source.sh install
```

## 2. Release assets

Для версии `1.3.0` используются:

```text
MDVWB-arm64-offline.tar.gz
MDVWB-arm64-offline.tar.gz.sha256
MDVWB-release-assets.sha256
online-install.sh
install_wirenboard.sh
```

`MDVWB-arm64-offline.tar.gz.sha256` проверяет архив package.
`MDVWB-release-assets.sha256` проверяет все опубликованные assets.

## 3. Предварительная проверка контроллера

```bash
dpkg --print-architecture
uname -m
cat /etc/os-release
ldconfig -p | grep 'libmosquitto\.so\.1'
systemctl status mosquitto.service --no-pager
date
timedatectl status
```

Ожидается:

```text
dpkg architecture: arm64
machine: aarch64
libmosquitto.so.1 присутствует
системное время корректно
```

Installer не устанавливает Mosquitto, не меняет `/mqtt`, nginx, TLS, firewall,
timezone, NTP и аппаратную настройку RS-485.

## 4. Online install

Скачайте из GitHub Release:

```text
online-install.sh
MDVWB-release-assets.sha256
```

Проверьте installer:

```bash
sha256sum -c MDVWB-release-assets.sha256 --ignore-missing
chmod +x online-install.sh
```

Установка конкретной версии:

```bash
sudo ./online-install.sh install --version 1.3.0
```

Обновление:

```bash
sudo ./online-install.sh update --version 1.3.0
```

Последний final release:

```bash
sudo ./online-install.sh install
```

Во время hardware validation release остаётся pre-release, поэтому указывайте
`--version 1.3.0`: путь `latest` pre-release не выбирает.

## 5. Offline install

Проверьте внешний checksum:

```bash
sha256sum -c MDVWB-arm64-offline.tar.gz.sha256
```

Распакуйте:

```bash
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
```

Проверьте package без изменения системы:

```bash
sudo ./offline-install.sh verify
sudo ./offline-install.sh install --dry-run
```

Установите:

```bash
sudo ./offline-install.sh install
```

Обновление:

```bash
sudo ./offline-install.sh update
```

## 6. Preflight

До остановки служб выполняются:

```text
package layout
manifest
version transition
internal SHA256SUMS
controller architecture
libmosquitto.so.1
binary self-tests
existing buses.json validation
```

Ошибка preflight не изменяет systemd и runtime-файлы.

## 7. Версии

Обычный upgrade разрешён.

Та же версия отклоняется:

```bash
sudo ./offline-install.sh update
```

Для repair:

```bash
sudo ./offline-install.sh update --force
```

Downgrade по умолчанию запрещён. Осознанный downgrade:

```bash
sudo ./offline-install.sh update --allow-downgrade
```

## 8. Backup и rollback

Перед install/update автоматически создаётся:

```text
/var/backups/mdvwb/<timestamp>-<pid>/
```

Другой каталог:

```bash
sudo ./offline-install.sh update --backup-dir /mnt/data/mdvwb-backups
```

Ручной backup:

```bash
sudo /usr/local/sbin/mdvwb-setup backup
```

Rollback последнего законченного backup:

```bash
sudo /usr/local/sbin/mdvwb-setup rollback
```

Конкретный backup:

```bash
sudo /usr/local/sbin/mdvwb-setup rollback \
  --backup /var/backups/mdvwb/<backup>
```

При ошибке после начала изменений installer автоматически восстанавливает
файлы и прежние `active/enabled` состояния служб.

`--no-backup` отключает также автоматический rollback и предназначен только для
диагностики.

## 9. Сохраняемые данные при update

```text
/etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
/etc/mdvwb/schedules.json
/etc/default/mdvwb-manager
/etc/default/mdvwb-scheduler
/var/lib/mdvwb/scheduler-state.tsv
/var/www/fancoils/assets/
```

Package обновляет binaries, systemd units, helper-файлы и обе web-страницы.

## 10. Состояние и журналы

Установленная версия:

```bash
sudo /usr/local/sbin/mdvwb-setup status
```

Полная health-проверка:

```bash
sudo /usr/local/sbin/mdvwb-setup status --health
```

Проверяются manager, scheduler и каждая шина с `enabled=true`.

Installation state:

```text
/var/lib/mdvwb/installation.json
```

Operation logs:

```text
/var/log/mdvwb/
```

## 11. Uninstall

Сначала план:

```bash
sudo /usr/local/sbin/mdvwb-setup uninstall --dry-run
```

Удаление приложения с сохранением пользовательских данных:

```bash
sudo /usr/local/sbin/mdvwb-setup uninstall --yes
```

Сохраняются конфигурации, environment-файлы, scheduler state, assets, журналы и
backup.

## 12. Purge

План:

```bash
sudo /usr/local/sbin/mdvwb-setup purge --dry-run
```

Удалить приложение и пользовательские данные, сохранив backup:

```bash
sudo /usr/local/sbin/mdvwb-setup purge --yes
```

Удалить также MDVWB-backup:

```bash
sudo /usr/local/sbin/mdvwb-setup purge --yes --remove-backups
```

Retained MQTT по умолчанию очищаются только для точных bus/address из валидного
`buses.json`. Сохранить retained:

```bash
sudo /usr/local/sbin/mdvwb-setup uninstall --yes --keep-retained
```

## 13. Runtime paths

```text
/usr/local/bin/MDVWB
/usr/local/bin/mdvwb-offline
/usr/local/bin/mdvwb-manager
/usr/local/bin/mdvwb-scheduler
/usr/local/sbin/mdvwb-setup
/usr/local/lib/mdvwb/

/etc/mdvwb/
/etc/default/mdvwb*
/etc/systemd/system/mdvwb*.service

/var/lib/mdvwb/
/var/backups/mdvwb/
/var/log/mdvwb/

/var/www/mdvwb/
/var/www/fancoils/
```

## 14. Проверка после установки

```bash
sudo /usr/local/sbin/mdvwb-setup status --health
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-offline --self-test
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json
systemctl --failed --no-pager
journalctl -u mdvwb-manager.service -u mdvwb-scheduler.service -n 200 --no-pager
```

Для каждой expected шины:

```bash
systemctl status mdvwb@1.service --no-pager
journalctl -u mdvwb@1.service -n 200 --no-pager
```

Web:

```text
http://<WB-address>/mdvwb/
http://<WB-address>/fancoils/
```

## 15. Основные коды завершения

```text
0  success
2  invalid arguments/package/configuration/state
3  installation state или backup отсутствует
4  та же версия без --force
5  downgrade без --allow-downgrade
6  architecture mismatch
7  health/service failure
8  automatic rollback failure
```

## 16. Диагностика

Package:

```bash
./offline-install.sh verify --package-only
./offline-install.sh version
```

Installed state:

```bash
/usr/local/sbin/mdvwb-setup status --health
```

Последние lifecycle logs:

```bash
ls -lt /var/log/mdvwb
tail -n 200 /var/log/mdvwb/*.log
```

Systemd:

```bash
systemctl status mdvwb-manager.service --no-pager
systemctl status mdvwb-scheduler.service --no-pager
systemctl list-units 'mdvwb@*.service' --all --no-pager
systemctl --failed --no-pager
```
