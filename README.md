# MDVWB

MDVWB is a standalone C++20 system for polling, controlling, scheduling, and
visualizing MDV XYE fan-coils through RS-485 and MQTT on Wiren Board.

Current project version: **1.3.0**.

MDVWB is not a `wb-mqtt-serial` module. Each physical RS-485 bus is owned by a
separate `MDVWB` process and a separate systemd instance.

## Components

The project builds four executables:

| Executable | Responsibility |
|---|---|
| `MDVWB` | Owns one serial port, polls one configured bus, executes commands, and publishes factual MQTT state |
| `mdvwb-offline` | Publishes retained offline state after a bus process stops |
| `mdvwb-manager` | Owns configuration, MQTT management APIs, systemd synchronization, dashboard uploads, and discovery |
| `mdvwb-scheduler` | Executes weekly, one-time, and manual schedules and waits for factual confirmation |

Two static browser applications are installed:

| URL | Purpose |
|---|---|
| `/mdvwb/` | Engineering configuration, bus control, discovery, and dashboard editing |
| `/fancoils/` | Operator panel, individual/group control, and schedules |

## Runtime model

```text
Browser
  ├─ /mdvwb/
  └─ /fancoils/
          |
          | MQTT over WebSocket: /mqtt
          v
      Mosquitto
          |
          ├─ mdvwb-manager.service
          ├─ mdvwb-scheduler.service
          ├─ mdvwb@1.service
          ├─ mdvwb@2.service
          └─ mdvwb@N.service
```

Important invariants:

- one driver process owns exactly one serial port;
- bus count is not hardcoded;
- separate buses operate independently;
- control uses individual addresses `0..63`;
- protocol broadcast `0xFF` is not used;
- factual controls are updated only from valid C0 reads;
- C3/CC/CD replies are validated but are not factual confirmation.

## Runtime files

```text
/usr/local/bin/MDVWB
/usr/local/bin/mdvwb-offline
/usr/local/bin/mdvwb-manager
/usr/local/bin/mdvwb-scheduler
/usr/local/sbin/mdvwb-setup
/usr/local/lib/mdvwb/

/etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
/etc/mdvwb/schedules.json
/etc/default/mdvwb-manager
/etc/default/mdvwb-scheduler
/etc/default/mdvwb-<bus>

/var/lib/mdvwb/installation.json
/var/lib/mdvwb/scheduler-state.tsv
/var/backups/mdvwb/
/var/log/mdvwb/

/var/www/mdvwb/
/var/www/fancoils/
/var/www/fancoils/assets/
```

## Build

### Windows / Visual Studio CMake

```powershell
cmake --preset x64-debug
cmake --build "out/build/x64-debug"
ctest --test-dir "out/build/x64-debug" -C Debug --output-on-failure
```

### Portable CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

A production build must require Mosquitto:

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMDVWB_REQUIRE_MOSQUITTO=ON
```

CMake currently registers 20 C++ tests. Deployment lifecycle shell tests are
kept under `tests/deploy/` and run in CI.

## Release assets

A tag matching the project version, for example `v1.3.0`, starts the native
ARM64 workflow. It builds inside Debian 11 Bullseye and publishes a GitHub
pre-release containing:

```text
MDVWB-arm64-offline.tar.gz
MDVWB-arm64-offline.tar.gz.sha256
MDVWB-release-assets.sha256
online-install.sh
install_wirenboard.sh
```

The release remains a pre-release until the real Wiren Board verification is
complete. The same release is then promoted to final/latest.

## Online installation

Download `online-install.sh` from the selected release, verify it with
`MDVWB-release-assets.sha256`, then run:

```bash
chmod +x online-install.sh
sudo ./online-install.sh install --version 1.3.0
```

Update:

```bash
sudo ./online-install.sh update --version 1.3.0
```

The online installer downloads the ready ARM64 package. It does not compile on
the controller.

## Offline installation

Copy the archive and checksum to the controller:

```bash
sha256sum -c MDVWB-arm64-offline.tar.gz.sha256
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
sudo ./offline-install.sh verify
sudo ./offline-install.sh install
```

Update:

```bash
sudo ./offline-install.sh update
```

## Lifecycle commands

```text
mdvwb-setup verify
mdvwb-setup status
mdvwb-setup backup
mdvwb-setup rollback
mdvwb-setup install
mdvwb-setup update
mdvwb-setup uninstall
mdvwb-setup purge
```

Install/update support:

```text
--dry-run
--force
--allow-downgrade
--backup-dir <directory>
--no-backup
```

Removal additionally supports:

```text
--yes
--keep-retained
--remove-backups
```

The installer performs package, architecture, library, self-test, and existing
configuration checks before stopping services. Updates create a backup and
automatically roll back on failure.

Existing user data is preserved during update:

```text
/etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
/etc/mdvwb/schedules.json
/etc/default/mdvwb-manager
/etc/default/mdvwb-scheduler
/var/lib/mdvwb/scheduler-state.tsv
/var/www/fancoils/assets/
```

## Verification

```bash
/usr/local/sbin/mdvwb-setup status --health
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-offline --self-test
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json
systemctl status mdvwb-manager.service --no-pager
systemctl status mdvwb-scheduler.service --no-pager
systemctl list-units 'mdvwb@*.service' --all --no-pager
```

Open:

```text
http://<WB-address>/mdvwb/
http://<WB-address>/fancoils/
```

## Source installation

Building directly on Wiren Board remains available for development only:

```bash
sudo ./deploy/install-from-source.sh install
```

`deploy/install_wirenboard.sh` is a compatibility wrapper for the online
installer.

## Documentation

| Document | Purpose |
|---|---|
| [`AGENTS.md`](AGENTS.md) | Repository map and non-negotiable invariants |
| [`docs/DEVELOPER.md`](docs/DEVELOPER.md) | Source architecture and extension rules |
| [`docs/INSTALLATION.md`](docs/INSTALLATION.md) | Complete installation and lifecycle guide |
| [`docs/WEB_AND_FANCOILS.md`](docs/WEB_AND_FANCOILS.md) | Engineering and operator web applications |
| [`docs/schedules-config.md`](docs/schedules-config.md) | Schedule schema and MQTT contract |
| [`docs/RELEASE_CHECKLIST.md`](docs/RELEASE_CHECKLIST.md) | Release and controller verification procedure |

The implementation and automated tests are the final source of truth when a
document disagrees with the current code.
