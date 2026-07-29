# MDVWB release checklist

> Финальная процедура выпуска MDVWB 1.2.0.
> Release считается завершённым только после проверки исходников, ARM64 artifact и реального Wiren Board.

## 1. Зафиксировать release scope

Запишите:

```text
release version
release commit SHA
branch
дата
оператор
target controller model
target OS
```

Проверьте, что version соответствует:

```text
CMakeLists.txt
README.md
artifact
MDVWB --version
```

Текущая project version:

```text
1.2.0
```

## 2. Проверить рабочее дерево Windows

Из:

```text
C:\Projects\MDVWB
```

```powershell
git status --short
git rev-parse HEAD
```

Release build должен происходить из ожидаемого commit.

## 3. Полный local CTest

```powershell
cmake --preset x64-debug
cmake --build "out/build/x64-debug"
ctest --test-dir "out/build/x64-debug" -C Debug --output-on-failure
```

Ожидается:

```text
20 tests
100% passed
```

Node.js не требуется для обязательного CTest release gate.

JavaScript model tests в `tests/web/` не входят в CTest.

## 4. Документация

```powershell
git diff --check
Select-String -Path "README.md","AGENTS.md","docs\*.md" -Pattern "TODO","TBD"
```

Проверьте ссылки на:

```text
docs/DEVELOPER.md
docs/INSTALLATION.md
docs/WEB_AND_FANCOILS.md
docs/schedules-config.md
docs/RELEASE_CHECKLIST.md
```

## 5. GitHub workflow `Validate MDVWB`

Проверьте successful run для release code.

Workflow выполняет:

- deploy shell syntax;
- default JSON parsing;
- required web files;
- Release build;
- full CTest;
- executable smoke.

Automatic trigger ограничен code/deploy/www paths.

Docs-only release commit может не запустить workflow автоматически.

В таком случае:

1. запустите workflow вручную;
2. либо зафиксируйте successful run на exact code commit;
3. не предполагайте success только по зелёному предыдущему commit.

## 6. ARM64 workflow

Запустите:

```text
Build ARM64 Offline Package
```

Input:

```text
create_artifact = true
```

Проверьте:

```text
runner = ubuntu-24.04-arm
machine = aarch64
container = debian:bullseye
```

Workflow обязан завершить:

```text
Release configure
build
full CTest
MDVWB --version
MDVWB --self-test
mdvwb-offline --self-test
manager validate
scheduler --help
package checksums
```

## 7. Скачать artifact

Artifact name:

```text
MDVWB-arm64-offline
```

Содержимое:

```text
MDVWB-arm64-offline.tar.gz
MDVWB-arm64-offline.tar.gz.sha256
```

Retention:

```text
30 days
```

Сохраните копию release artifact вне GitHub retention window.

## 8. Проверить внешний checksum

Windows:

```powershell
Get-FileHash "C:\Users\pereverworkki\Downloads\MDVWB-arm64-offline.tar.gz" -Algorithm SHA256
Get-Content "C:\Users\pereverworkki\Downloads\MDVWB-arm64-offline.tar.gz.sha256"
```

Linux/WB:

```bash
sha256sum -c MDVWB-arm64-offline.tar.gz.sha256
```

Ожидается:

```text
OK
```

## 9. Проверить внутренний package

```bash
rm -rf MDVWB-arm64
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
sha256sum -c SHA256SUMS
```

Проверьте наличие:

```text
MDVWB
mdvwb-offline
mdvwb-manager
mdvwb-scheduler
offline-install.sh
mdvwb-run
mdvwb@.service
mdvwb-manager.service
mdvwb-scheduler.service
buses.example.json
dashboard.default.json
schedules.default.json
www/mdvwb/
www/fancoils/
docs/RELEASE_CHECKLIST.md
```

## 10. Проверить executable architecture

На Linux ARM64 package host:

```bash
file MDVWB mdvwb-offline mdvwb-manager mdvwb-scheduler
readelf -h MDVWB | grep -E 'Class:|Machine:'
```

Ожидается ELF64 AArch64.

## 11. Подготовить controller

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
arm64
aarch64
correct local time
```

## 12. Создать backup

До installation:

```bash
BACKUP="/root/mdvwb-release-backup-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BACKUP"
cp -a /etc/mdvwb "$BACKUP/" 2>/dev/null || true
cp -a /etc/default/mdvwb-manager "$BACKUP/" 2>/dev/null || true
cp -a /etc/default/mdvwb-scheduler "$BACKUP/" 2>/dev/null || true
cp -a /etc/default/mdvwb-* "$BACKUP/" 2>/dev/null || true
cp -a /var/lib/mdvwb "$BACKUP/" 2>/dev/null || true
mkdir -p "$BACKUP/fancoils"
cp -a /var/www/fancoils/assets "$BACKUP/fancoils/" 2>/dev/null || true
```

Запишите previous package/version.

Installer не создаёт backup и не выполняет automatic rollback.

## 13. Install release

```bash
cd /root/MDVWB-arm64
chmod +x offline-install.sh
./offline-install.sh
```

Installer должен завершиться exit code `0`.

Сохраните полный terminal output.

## 14. Binary smoke

```bash
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-offline --self-test
/usr/local/bin/mdvwb-manager --help
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-scheduler --help
```

## 15. systemd smoke

```bash
systemctl status mdvwb-manager.service --no-pager
systemctl status mdvwb-scheduler.service --no-pager
systemctl list-units 'mdvwb@*.service' --all --no-pager
systemctl --failed --no-pager
```

Проверьте каждый expected bus.

Installer автоматически проверяет только manager и scheduler.

## 16. Logs после start

```bash
journalctl \
  -u mdvwb-manager.service \
  -u mdvwb-scheduler.service \
  -n 200 \
  --no-pager
```

Для каждой test bus:

```bash
journalctl -u mdvwb@1.service -n 200 --no-pager
```

Не должно быть restart loop.

## 17. Web package smoke

```bash
test -f /var/www/mdvwb/index.html
test -f /var/www/mdvwb/dashboard-editor.js
test -f /var/www/fancoils/index.html
test -f /var/www/fancoils/app.js
test -f /var/www/fancoils/schedule-model.js
test -f /var/www/fancoils/scheduler-status-ui.js
test -f /var/www/fancoils/scheduler-status-health.js
```

Откройте:

```text
http://<WB-address>/mdvwb/
http://<WB-address>/fancoils/
```

## 18. MQTT/WebSocket smoke

Проверьте:

- browser MQTT connected;
- `/mqtt` WebSocket существует;
- retained config получен;
- `/mdvwb/scheduler/status` поступает;
- controller clock корректен в scheduler payload или CLI.

Current `/fancoils/` entry point не загружает scheduler-status helpers, поэтому отдельный clock badge и live-heartbeat freshness gate не являются действующими release criteria.

CLI:

```bash
mosquitto_sub -C 1 -v -t '/mdvwb/status'
mosquitto_sub -C 1 -v -t '/mdvwb/scheduler/status'
```

## 19. Configuration smoke

Проверьте:

```text
buses revision
dashboard revision
schedules revision
default panel
referenceIssues
enabled bus list
actual serial ports
actual addresses
```

`buses.example.json` не должен случайно остаться production configuration.

## 20. Hardware C0 smoke

Для test fan:

- metadata опубликована;
- `Status` factual;
- `Temp` factual либо пустая при unavailable T1;
- `Mode` factual;
- `Speed` factual;
- `SetTemp` factual;
- device не offline при valid replies.

## 21. Individual command smoke

Проверьте по одному:

```text
Power
Mode
Speed
SetTemp
Blinds
Blok
```

Требования:

- command topic non-retained;
- UI не подменяет factual state;
- confirmation приходит base topic;
- C3 response не считается factual;
- SetTemp Auto не создаёт infinite retry.

## 22. Group command smoke

Проверьте:

- выбраны только нужные devices;
- изменяются только selected controls;
- offline/waiting targets пропускаются;
- confirmations считаются отдельно;
- broadcast отсутствует.

## 23. Offline publisher smoke

Terminal 1:

```bash
mosquitto_sub -v \
  -t '/devices/Fan-1_1/controls/Alarm' \
  -t '/devices/Fan-1_1/controls/Status'
```

Terminal 2:

```bash
systemctl stop mdvwb@1.service
```

Ожидается:

```text
Alarm=2
Status=7
```

Затем:

```bash
systemctl start mdvwb@1.service
```

## 24. Dashboard smoke

Проверьте:

- `/mdvwb/#dashboard`;
- multiple panels;
- direct panel URL;
- placement move;
- user number;
- visible;
- save revision increment;
- two-browser conflict;
- current draft preservation;
- background upload;
- wrong SHA rejection;
- concurrent revision conflict;
- old asset preservation/removal rules.

## 25. Schedule smoke

Проверьте:

- weekly save;
- once save;
- target selection only visible;
- manual disabled schedule;
- manager queued result;
- scheduler result с `origin="scheduler"` через MQTT CLI;
- executing;
- completed;
- timeout display;
- current page показывает последнее полученное result;
- automatic test at controller local time;
- missed once does not execute late.

## 26. Scheduler freshness smoke

Во время test:

1. измените test dependency безопасно;
2. убедитесь, что invalid bus/dashboard reference блокирует run;
3. убедитесь, что active run не завершается stale fact;
4. восстановите valid dependency;
5. убедитесь, что scheduler автоматически вернулся в ready.

Не выполняйте этот test на production targets без окна обслуживания.

## 27. Discovery smoke

Для test bus:

- selected service остановлен;
- result содержит expected address;
- service не запускается автоматически;
- same-bus duplicate отклоняется;
- другая bus остаётся независимой.

Discovery занимает три прохода `0..63`.

## 28. Update preservation smoke

Сравните backup и current:

```bash
ls -l /etc/mdvwb
ls -l /var/www/fancoils/assets
```

Проверьте, что сохранены:

```text
buses.json
dashboard.json
schedules.json
uploaded assets
manager env customization
scheduler env customization
```

Проверьте custom `mdvwb.env`: package заменяет его.

## 29. Failure and rollback readiness

До завершения release smoke должны оставаться:

```text
previous known-good package
configuration backup
assets backup
scheduler state backup
```

Rollback binaries/web:

```bash
cd /root/MDVWB-arm64-previous
./offline-install.sh
```

Полный rollback требует восстановления backup JSON/state/assets.

## 30. Release evidence

Сохраните:

```text
release commit SHA
Validate workflow URL/status
ARM64 workflow URL/status
artifact SHA-256
controller model
controller OS
installer output
systemctl output
CTest output
hardware smoke result
operator name
date/time
known limitations
```

## 31. Known deployment limitation

`deploy/install_wirenboard.sh` в текущем commit не устанавливает operator application `/fancoils/` и explicit dashboard/schedules defaults.

`scheduler-status-ui.js` импортирует `scheduler-status-health.js`, но current `/fancoils/index.html` загружает только `app.js`, а `app.js` status UI helper не импортирует. Live-heartbeat gate, WB clock badge и 90-second direct-result bridge пока не являются production behavior.

Offline installer required-file list и CI workflows также не везде явно проверяют `scheduler-status-health.js`, хотя artifact копирует весь `www/fancoils/`.

Production release проверяется только через offline package.

Не используйте online installer или наличие dormant helper files как доказательство полноты browser integration.

## 32. Release gate

Release запрещён, если выполняется хотя бы одно:

```text
CTest failed
checksum failed
wrong architecture
manager inactive
scheduler inactive
expected bus failed
/mqtt unavailable
factual C0 absent
offline publisher failed
configuration lost
assets lost
manual schedule has no terminal result
automatic schedule uses wrong controller time
rollback files absent before smoke completion
```

## 33. Final sign-off

```text
[ ] Exact release SHA recorded
[ ] Local CTest 20/20
[ ] Validate workflow successful
[ ] ARM64 workflow successful
[ ] External checksum OK
[ ] Internal checksums OK
[ ] Controller backup created
[ ] Installer exit 0
[ ] Binary smoke OK
[ ] Manager active
[ ] Scheduler active
[ ] All expected bus services checked
[ ] Both web apps open
[ ] /mqtt connected
[ ] Controller clock correct in CLI/scheduler payload
[ ] Current scheduler-status UI limitation recorded
[ ] C0 factual state OK
[ ] Individual command OK
[ ] Group command OK
[ ] Offline publisher OK
[ ] Dashboard save/upload OK
[ ] Manual schedule terminal result
[ ] Automatic schedule OK
[ ] Configuration preserved
[ ] Assets preserved
[ ] Previous package retained
[ ] Release evidence archived
```
