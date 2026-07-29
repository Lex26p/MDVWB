# MDVWB 1.3.0 release checklist

> Tag `v1.3.0` создаёт GitHub pre-release. Final/latest разрешён только после
> проверки package и реального Wiren Board.

## 1. Release identity

Запишите:

```text
version: 1.3.0
commit SHA:
tag: v1.3.0
branch:
date/time:
operator:
controller model:
controller OS:
```

Версия должна совпадать в:

```text
CMakeLists.txt
README.md
docs/INSTALLATION.md
package manifest.json
MDVWB --version
tag v1.3.0
```

## 2. Local repository

```powershell
cd C:\Projects\MDVWB
git status --short
git rev-parse HEAD
git diff --check
```

Рабочее дерево должно быть чистым перед созданием tag.

## 3. Local C++ gate

```powershell
cmake --preset x64-debug
cmake --build "out/build/x64-debug"
ctest --test-dir "out/build/x64-debug" -C Debug --output-on-failure
```

Ожидается:

```text
20/20 C++ tests passed
```

## 4. Validate workflow

На exact release commit должен успешно завершиться:

```text
Validate MDVWB
```

Он проверяет:

```text
deployment shell syntax
7 deployment lifecycle test suites
default JSON
release contract
Release C++ build
20 C++ tests
binary smoke
```

## 5. Создать и отправить tag

```powershell
git tag -a v1.3.0 -m "MDVWB 1.3.0"
git push origin v1.3.0
```

Tag обязан точно совпадать с `project(MDVWB VERSION ...)`. Иначе ARM64 workflow
завершится до package build.

## 6. ARM64 workflow

Tag автоматически запускает:

```text
Build ARM64 Offline Package
```

Ожидается:

```text
runner: ubuntu-24.04-arm
machine: aarch64
container: debian:bullseye
full deployment tests
Release configure/build
20 C++ tests
binary smoke
package manifest
internal SHA256SUMS
outer archive checksum
release asset checksum
```

## 7. Pre-release assets

GitHub pre-release `v1.3.0` должен содержать ровно необходимые assets:

```text
MDVWB-arm64-offline.tar.gz
MDVWB-arm64-offline.tar.gz.sha256
MDVWB-release-assets.sha256
online-install.sh
install_wirenboard.sh
```

Повторный запуск workflow обновляет assets того же release.

## 8. Проверить published checksums

Скачайте все assets в один каталог.

Windows:

```powershell
Get-FileHash ".\MDVWB-arm64-offline.tar.gz" -Algorithm SHA256
Get-Content ".\MDVWB-arm64-offline.tar.gz.sha256"
Get-Content ".\MDVWB-release-assets.sha256"
```

Linux:

```bash
sha256sum -c MDVWB-release-assets.sha256
sha256sum -c MDVWB-arm64-offline.tar.gz.sha256
```

## 9. Проверить package

```bash
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
sha256sum -c SHA256SUMS
./offline-install.sh verify --package-only
./offline-install.sh version
```

Проверьте:

```text
version=1.3.0
architecture=arm64
commit=release commit SHA
```

## 10. Проверить ELF

```bash
file MDVWB mdvwb-offline mdvwb-manager mdvwb-scheduler
readelf -h MDVWB | grep -E 'Class:|Machine:'
```

Ожидается ELF64 AArch64.

## 11. Controller preflight

```bash
dpkg --print-architecture
uname -m
cat /etc/os-release
ldconfig -p | grep 'libmosquitto\.so\.1'
systemctl status mosquitto.service --no-pager
date
timedatectl status
```

## 12. Зафиксировать исходное состояние

```bash
/usr/local/sbin/mdvwb-setup status --health 2>/dev/null || true
systemctl list-units 'mdvwb@*.service' --all --no-pager
ls -la /etc/mdvwb 2>/dev/null || true
ls -la /var/www/fancoils/assets 2>/dev/null || true
```

Сохраните пользовательские значения, которые должны пережить update.

## 13. Offline dry-run

```bash
cd /root/MDVWB-arm64
sudo ./offline-install.sh verify
sudo ./offline-install.sh update --dry-run
```

Для чистого контроллера используйте `install --dry-run`.

## 14. Offline install/update

```bash
sudo ./offline-install.sh update
```

Либо:

```bash
sudo ./offline-install.sh install
```

Ожидается:

```text
BACKUP_CREATED
MDVWB_RESULT=success
method=offline
version=1.3.0
```

## 15. Installed health

```bash
sudo /usr/local/sbin/mdvwb-setup status --health
```

Ожидается:

```text
HEALTH=OK
```

Проверяются manager, scheduler и все `enabled=true` bus services.

## 16. Preservation

Проверьте, что сохранены:

```text
buses.json
dashboard.json
schedules.json
manager env customization
scheduler env customization
scheduler-state.tsv
uploaded assets
```

Найдите automatic backup и operation log:

```bash
ls -lt /var/backups/mdvwb
ls -lt /var/log/mdvwb
```

## 17. Binary and systemd smoke

```bash
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-offline --self-test
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json
systemctl --failed --no-pager
```

Для каждой expected bus:

```bash
systemctl status mdvwb@1.service --no-pager
journalctl -u mdvwb@1.service -n 200 --no-pager
```

## 18. Web and MQTT smoke

Откройте:

```text
http://<WB-address>/mdvwb/
http://<WB-address>/fancoils/
```

Проверьте:

```text
/mqtt connected
configuration loaded
dashboard opened
schedule status received
no browser console errors
```

## 19. Hardware smoke

Для test fan проверьте factual C0:

```text
Power
Mode
Speed
SetTemp
Temp
Blinds
Blok
Alarm
Status
```

Проверьте individual commands:

```text
Power
Mode
Speed
SetTemp
Blinds
Blok
```

Требования:

```text
command topic non-retained
C3 is not factual confirmation
confirmation arrives on base topic after C0
no broadcast
```

## 20. Group, schedule and discovery smoke

Проверьте:

```text
selected group targets only
selected controls only
weekly/once/manual schedules
terminal schedule result
controller local time
three-pass discovery 0..63
selected bus remains stopped after discovery
other buses remain independent
```

## 21. Rollback smoke

Используйте созданный backup:

```bash
sudo /usr/local/sbin/mdvwb-setup rollback
sudo /usr/local/sbin/mdvwb-setup status --health
```

Затем повторно установите `1.3.0`.

Проверьте восстановление files, configurations, assets и service states.

## 22. Online smoke against pre-release

Скачайте `online-install.sh` и checksum assets.

Проверка:

```bash
sha256sum -c MDVWB-release-assets.sha256 --ignore-missing
chmod +x online-install.sh
```

Online repair той же версии:

```bash
sudo ./online-install.sh update --version 1.3.0 --force
```

Ожидается:

```text
method=online
HEALTH=OK
```

## 23. Uninstall smoke

```bash
sudo /usr/local/sbin/mdvwb-setup uninstall --dry-run
```

Проверяйте real uninstall только если предусмотрено тестовое окно:

```bash
sudo /usr/local/sbin/mdvwb-setup uninstall --yes
```

Должны сохраниться configuration, scheduler state, assets, logs и backups.

После проверки снова установите release.

## 24. Promotion to final

Только после всех проверок:

```bash
gh release edit v1.3.0 \
  --prerelease=false \
  --latest \
  --title "MDVWB 1.3.0"
```

То же можно сделать через GitHub Release UI.

Проверьте:

```text
release is not draft
release is not prerelease
release is latest
all five assets present
```

## 25. Release evidence

Сохраните:

```text
release commit SHA
tag
Validate workflow URL/status
ARM64 workflow URL/status
all asset SHA-256 values
controller model and OS
offline installer output
online installer output
status --health output
systemctl output
CTest output
hardware smoke result
rollback result
operator
date/time
known limitations
```

## Final gate

Release запрещён, если выполняется хотя бы одно:

```text
tag/version mismatch
Validate failed
ARM64 build failed
checksum failed
wrong architecture
HEALTH=FAILED
expected bus inactive
configuration lost
assets lost
automatic rollback failed
/mqtt unavailable
factual C0 absent
hardware command failed
online exact-version install failed
```

## Sign-off

```text
[ ] Exact release SHA recorded
[ ] Tag v1.3.0 pushed
[ ] Validate successful
[ ] ARM64 workflow successful
[ ] Five pre-release assets present
[ ] External and internal checksums OK
[ ] Package version/commit/architecture correct
[ ] Offline install/update successful
[ ] HEALTH=OK
[ ] User data preserved
[ ] Backup and operation log created
[ ] Rollback successful
[ ] Both web applications open
[ ] MQTT/WebSocket connected
[ ] Hardware factual C0 successful
[ ] Individual commands successful
[ ] Group control successful
[ ] Scheduler successful
[ ] Discovery successful
[ ] Online repair successful
[ ] Uninstall preservation verified or explicitly deferred
[ ] Release promoted to final/latest
[ ] Evidence archived
```
