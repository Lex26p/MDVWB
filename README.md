# MDVWB

MDVWB is a standalone C++20 system for polling, controlling, scheduling, and visualizing MDV XYE fan-coils through RS-485 and MQTT on Wiren Board.

Current project version: **1.2.0**.

MDVWB is not a `wb-mqtt-serial` module. Each physical RS-485 bus is owned by a separate `MDVWB` process and a separate systemd instance.

## What the project contains

The project builds four executables:

| Executable | Responsibility |
|---|---|
| `MDVWB` | Owns one serial port, polls one configured bus, executes fan-coil commands, and publishes factual MQTT state |
| `mdvwb-offline` | Publishes retained offline state for all configured addresses after a bus process stops or exits unexpectedly |
| `mdvwb-manager` | Owns configuration files, MQTT management APIs, systemd synchronization, dashboard uploads, and device discovery coordination |
| `mdvwb-scheduler` | Executes weekly, one-time, and manual schedules and waits for factual MQTT confirmation |

Two static browser applications are installed:

| URL | Source directory | Purpose |
|---|---|---|
| `/mdvwb/` | `www/mdvwb/` | Engineering configuration, bus service control, discovery, and dashboard editing |
| `/fancoils/` | `www/fancoils/` | Operator panel, individual and group control, and schedule editing/execution |

## Runtime architecture

```text
Browser
  ├─ /mdvwb/      engineering application
  └─ /fancoils/   operator application
          |
          | MQTT over WebSocket: /mqtt
          v
      Mosquitto
          |
          ├─ mdvwb-manager.service
          |    ├─ /etc/mdvwb/buses.json
          |    ├─ /etc/mdvwb/dashboard.json
          |    ├─ /etc/mdvwb/schedules.json
          |    ├─ /etc/default/mdvwb-<bus>
          |    ├─ mdvwb@<bus>.service lifecycle
          |    ├─ dashboard image uploads
          |    └─ discovery workers isolated by bus ID
          |
          ├─ mdvwb-scheduler.service
          |    ├─ reads all three JSON configurations
          |    ├─ publishes individual /on1 commands
          |    └─ waits for factual base-topic confirmation
          |
          ├─ mdvwb@1.service ── MDVWB ── serial port for bus 1
          ├─ mdvwb@2.service ── MDVWB ── serial port for bus 2
          └─ mdvwb@N.service ── MDVWB ── serial port for bus N
```

The process model is intentional:

- one driver process owns exactly one serial port;
- bus count is not hardcoded;
- separate buses poll independently;
- a failure or discovery operation on one bus must not stop another bus;
- systemd owns restart policy and process logging;
- control uses individual addresses `0..63`; protocol broadcast `0xFF` is not used.

## Runtime files

### Executables and helper

```text
/usr/local/bin/MDVWB
/usr/local/bin/mdvwb-offline
/usr/local/bin/mdvwb-manager
/usr/local/bin/mdvwb-scheduler
/usr/local/lib/mdvwb/mdvwb-run
/usr/local/lib/mdvwb/mdvwb.env
```

### Configuration and state

```text
/etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
/etc/mdvwb/schedules.json
/etc/default/mdvwb-manager
/etc/default/mdvwb-scheduler
/etc/default/mdvwb-<bus>
/var/lib/mdvwb/scheduler-state.tsv
```

### systemd units

```text
/etc/systemd/system/mdvwb@.service
/etc/systemd/system/mdvwb-manager.service
/etc/systemd/system/mdvwb-scheduler.service
```

### Web applications

```text
/var/www/mdvwb/
/var/www/fancoils/
/var/www/fancoils/assets/
```

Uploaded dashboard backgrounds are preserved under `/var/www/fancoils/assets` during an offline update.

## Core behavior

### Fan-coil state and commands

A configured device is named:

```text
Fan-<bus>_<address>
```

Commands are non-retained and use the `/on1` suffix:

```text
/devices/Fan-1_3/controls/Power/on1
/devices/Fan-1_3/controls/Mode/on1
/devices/Fan-1_3/controls/Speed/on1
/devices/Fan-1_3/controls/SetTemp/on1
/devices/Fan-1_3/controls/Blinds/on1
/devices/Fan-1_3/controls/Blok/on1
```

Factual state is published retained to the corresponding base control topics. `Mode`, `Speed`, `SetTemp`, and other factual controls are updated only from a valid C0 read response. C3/CC/CD replies are validated but are not treated as confirmed state.

When a bus service stops or the driver exits unexpectedly, `ExecStopPost` runs `mdvwb-offline`, which publishes retained offline state for the configured fan coils before systemd restarts the process.

### Bus configuration

Canonical path:

```text
/etc/mdvwb/buses.json
```

Schema version is `1`. The configuration includes an optimistic-concurrency `revision` field in canonical manager output.

```json
{
  "version": 1,
  "revision": 0,
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

Important rules:

- bus ID: `1..999`;
- fan-coil address: `0..63`;
- bus IDs and serial ports are unique;
- addresses are unique within a bus;
- a port must be a safe absolute path beginning with `/dev/`;
- an enabled bus must contain at least one address;
- unknown JSON fields are rejected;
- `/etc/default/mdvwb-<bus>` files are generated runtime derivatives, not the source of truth.

### Dashboard configuration

Canonical path:

```text
/etc/mdvwb/dashboard.json
```

The current collection schema version is `2`. It supports up to 64 independent panels, a `defaultPanel`, panel-specific background images, fan-coil placement, user-facing numbers, and optimistic revision checks.

Background files are uploaded through sequential binary MQTT chunks. The manager verifies size, SHA-256, actual PNG/JPEG/WebP headers, image dimensions, panel identity, and the dashboard revision again when the upload finishes. A concurrent dashboard save prevents an older upload from overwriting the newer configuration.

### Schedules

Canonical path:

```text
/etc/mdvwb/schedules.json
```

`mdvwb-scheduler` executes weekly and one-time schedules and accepts manual run requests. Before execution it validates references against the current bus and dashboard configurations. It also detects content changes even when file size and timestamp are unchanged.

Commands are published per fan coil; broadcast is never used. Completion is based on factual base MQTT topics rather than optimistic command echo.

### Discovery

Discovery scans addresses `0..63` for three complete passes. One valid C0 response is enough to include an address in the result.

- the selected bus service is stopped before scanning;
- the service remains stopped when discovery finishes;
- discovered addresses are published but never applied automatically;
- the same bus cannot run two discovery operations concurrently;
- different buses may be discovered concurrently.

### Legacy migration

`mdvwb-manager migrate-defaults` converts legacy `/etc/default/mdvwb` and `/etc/default/mdvwb-N` files without executing their contents.

Migration is fail-fast. Invalid assignments, duplicate `MDVWB_*` variables, missing required fields, malformed quoting, non-canonical file names, filename/`MDVWB_BUS` disagreement, and multiple sources for one bus are errors. A partial or ambiguous result is not silently written.

## Build

### Windows and Visual Studio CMake

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
ctest --test-dir out/build/x64-debug -C Debug --output-on-failure
```

Available presets also include `x64-release`, `x86-debug`, `x86-release`, `linux-debug`, and `macos-debug` where their host conditions are satisfied.

### Portable CMake

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

A production build must require Mosquitto support:

```bash
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMDVWB_REQUIRE_MOSQUITTO=ON
```

Without `MDVWB_REQUIRE_MOSQUITTO=ON`, local protocol and configuration work can be built without libmosquitto.

## Test suite

CMake registers 20 tests:

```text
mdv_protocol_self_test
mdvwb_offline_publisher_test
mdvwb_mqtt_delivery_test
mdvwb_driver_fairness_test
mdv_buses_config_test
mdvwb_dashboard_config_test
mdvwb_schedules_config_test
mdvwb_dashboard_upload_test
mdvwb_manager_cli_test
mdvwb_service_sync_test
mdvwb_manager_mqtt_test
mdvwb_manager_revision_test
mdvwb_dashboard_concurrency_test
mdvwb_manager_transaction_test
mdvwb_discovery_runner_test
mdvwb_discovery_async_test
mdvwb_migration_test
mdvwb_scheduler_test
mdvwb_scheduler_freshness_test
mdvwb_mqtt_command_delivery_test
```

Additional JavaScript model tests are stored under `tests/web/` and are not part of CTest.

## Continuous integration and release package

`.github/workflows/validate.yml` performs a Release build with required Mosquitto support, validates deployment files and default JSON, runs the full CTest suite, and performs executable smoke checks.

`.github/workflows/build-arm64-offline.yml` runs manually on a native ARM64 runner, builds inside Debian 11 Bullseye, runs the full test suite, and produces:

```text
MDVWB-arm64-offline.tar.gz
MDVWB-arm64-offline.tar.gz.sha256
```

The package includes all four executables, systemd units, environment templates, safe default JSON files, both web applications, the offline installer, internal checksums, and the release checklist.

## Offline installation

After copying the artifact to a Wiren Board ARM64 controller:

```bash
cd /root
sha256sum -c MDVWB-arm64-offline.tar.gz.sha256
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
./offline-install.sh
```

The installer requires root, `arm64`, and `libmosquitto.so.1`. It preserves existing non-empty:

```text
/etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
/etc/mdvwb/schedules.json
/var/www/fancoils/assets/
```

It validates internal checksums, installs all runtime components, runs self-tests, applies bus configuration, and starts the manager and scheduler services.

## Basic verification

```bash
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-offline --self-test
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-scheduler --help
systemctl status mdvwb-manager.service --no-pager
systemctl status mdvwb-scheduler.service --no-pager
systemctl list-units 'mdvwb@*.service' --all --no-pager
```

Open:

```text
http://<WB-address>/mdvwb/
http://<WB-address>/fancoils/
```

## Documentation

| Document | Purpose |
|---|---|
| [`AGENTS.md`](AGENTS.md) | Repository map, non-negotiable invariants, test ownership, and guidance for coding agents |
| [`docs/DEVELOPER.md`](docs/DEVELOPER.md) | Detailed source architecture, protocol, MQTT, manager, dashboard, scheduler, and extension rules |
| [`docs/INSTALLATION.md`](docs/INSTALLATION.md) | Installation, update, configuration, operation, recovery, and diagnostics |
| [`docs/WEB_AND_FANCOILS.md`](docs/WEB_AND_FANCOILS.md) | Engineering and operator web applications |
| [`docs/schedules-config.md`](docs/schedules-config.md) | Schedule schema and MQTT contract |
| [`docs/RELEASE_CHECKLIST.md`](docs/RELEASE_CHECKLIST.md) | Release and controller smoke-test procedure |

The implementation and automated tests are the final source of truth when a document disagrees with the current code.
