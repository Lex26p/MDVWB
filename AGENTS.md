# MDVWB AI Agent Context

> AI-only repository context. This file is not end-user documentation.
> Treat protocol invariants, MQTT contracts, runtime paths, and migration rules below as mandatory unless verified against current source and real hardware.

## 1. Project identity

- Project: `MDVWB`.
- Current integrated version: `1.2.0`.
- Language: C++20.
- Target: Wiren Board ARM64, Debian 11 Bullseye, glibc 2.31.
- Development host: Windows, Visual Studio CMake, PowerShell.
- Repository: `https://github.com/Lex26p/MDVWB`.
- Typical local path: `C:\Projects\MDVWB`.
- Purpose: standalone multi-bus MDV XYE fan-coil driver using RS-485 and MQTT.
- This is not a `wb-mqtt-serial` module.
- The old C# repository was used only as behavioral/protocol reference. Do not port its architecture back into this project.

## 2. Current completion state

Implemented and tested:

- fixed-size MDV request/response framing;
- request construction and response validation;
- cross-platform serial transport;
- one common transaction pacer for all command types;
- round-robin polling;
- cached complete C3 command frame per device;
- MQTT command routing;
- retained factual state publishing only on change;
- lock/unlock support;
- discovery over addresses `0..63` for three passes;
- arbitrary number of independent buses;
- one systemd process per bus;
- shared JSON bus configuration;
- independent revisioned dashboard configuration;
- service synchronization through `mdvwb-manager`;
- MQTT manager API;
- static web configuration UI;
- automatic Wiren Board MQTT metadata publication;
- migration from legacy `/etc/default/mdvwb-N` files;
- cleanup of obsolete retained device topics;
- offline ARM64 package workflow.

Do not reintroduce:

- hardcoded support for only one or two buses;
- a shared multithreaded process owning multiple serial ports;
- the old `ArrID` wb-rules virtual-device script;
- broadcast control with address `0xFF`;
- command topics ending in `/on` instead of `/on1`;
- state publishing on command topics;
- non-retained factual state;
- automatic application of discovery results;
- automatic restart of a bus after discovery;
- `/mnt/data/www/mdvwb` as the default web root.

## 3. Runtime architecture

```text
Browser: /var/www/mdvwb
        |
        | MQTT over WebSocket: /mqtt
        v
Mosquitto broker
        |
        +--> mdvwb-manager.service
        |      |- reads/writes /etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
        |      |- validates configuration
        |      |- synchronizes /etc/default/mdvwb-N
        |      |- controls mdvwb@N.service
        |      `- runs discovery for one selected bus
        |
        +--> mdvwb@1.service --> one MDVWB process --> one RS-485 port
        +--> mdvwb@2.service --> one MDVWB process --> one RS-485 port
        `--> mdvwb@N.service --> one MDVWB process --> one RS-485 port
```

Process model is intentional:

- exactly one driver process owns one serial port;
- buses poll simultaneously because processes run independently;
- a crash or discovery on one bus must not stop other buses;
- systemd owns restart and logs;
- separate processes are preferred over threads for fault isolation and simple port ownership.

## 4. Important runtime paths

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
/var/www/mdvwb/
```

The correct default web directory for the tested Wiren Board installation is:

```text
/var/www/mdvwb
```

The page URL is:

```text
http://<WB-address>/mdvwb/
```

Do not change the standard Wiren Board web routing. The trailing slash should be used.

## 5. Repository layout

Driver sources are under `src/driver/`:

- `src/driver/MDVWB.cpp`, `src/driver/MDVWB.h`: executable entry point, run modes, self-test orchestration.
- `src/driver/mdv_protocol.*`: frame construction, validation, response parsing, frame collector.
- `src/driver/mdv_serial.*`: serial port, 4800 8N1 transport, pacing, transaction execution.
- `src/driver/mdv_device.*`: per-device confirmed state, cached complete C3 frame, pending fields.
- `src/driver/mdv_driver.*`: queues, polling order, C3/CC/CD execution, confirmation reads.
- `src/driver/mdv_discovery.*`: three-pass sequential scan.
- `src/driver/mdv_mqtt.*`: fan-coil command router, state publishing, system-device state.
- `src/driver/mdv_metadata.*`: retained Wiren Board device/control metadata.
- `src/driver/mdv_mosquitto.*`: libmosquitto implementation of `IMqttClient`; shared with the manager target.
- `src/driver/mdv_config.*`: driver CLI parsing and validation.

Manager sources are under `src/manager/`:

- `src/manager/mdv_buses_config.*`: strict bus JSON parser, validator, canonical serializer.
- `src/manager/mdv_dashboard_config.*`: strict dashboard JSON parser, validator, serializer, and bus/address reference checks.
- `src/manager/mdvwb_dashboard_upload.*`: chunked binary image upload, SHA-256, image header validation, and temporary asset handling.
- `src/manager/mdvwb_service_sync.*`: plan/apply logic for `/etc/default/mdvwb-N` and systemd.
- `src/manager/mdvwb_manager_cli.*`: manager commands.
- `src/manager/mdvwb_manager_main.cpp`: manager executable entry point.
- `src/manager/mdvwb_manager_mqtt.*`: MQTT configuration/control daemon.
- `src/manager/mdvwb_discovery_runner.*`: starts driver discovery and parses its result.
- `src/manager/mdvwb_migration.*`: converts legacy per-bus environment files to `buses.json`.

Deployment:

- `deploy/mdvwb@.service`;
- `deploy/mdvwb-manager.service`;
- `deploy/mdvwb-run`;
- `deploy/mdvwb.env`;
- `deploy/mdvwb-manager.env`;
- `deploy/install_wirenboard.sh`;
- `deploy/offline-install.sh`;
- `deploy/buses.example.json`.

Web UI:

- `www/mdvwb/index.html`;
- `www/mdvwb/app.js`;
- `www/mdvwb/model.js`;
- `www/mdvwb/dashboard-editor.js`;
- `www/mdvwb/dashboard-model.js`;
- `www/mdvwb/dashboard-placement-editor.js`;
- `www/mdvwb/mqtt-client.js`;
- `www/mdvwb/styles.css`.

Tests:

- manager/configuration C++ tests are under `tests/manager/`;
- `tests/web/mdvwb_web_model_test.mjs` is the Node.js web-model test;
- protocol/driver self-tests remain inside the `MDVWB --self-test` executable.

Obsolete stage documents such as `STEP02.md` are not project documentation and should not be restored.

## 6. Shared bus configuration

Canonical path:

```text
/etc/mdvwb/buses.json
```

Schema version is exactly `1`:

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
      "enabled": false,
      "port": "/dev/ttyUSB0",
      "addresses": []
    }
  ]
}
```

Validation rules:

- root fields: only `version`, `buses`;
- bus fields: only `id`, `enabled`, `port`, `addresses`;
- bus ID range: `1..999`;
- bus IDs must be unique;
- each port must be unique;
- port must be a safe absolute path beginning with `/dev/`;
- allowed path characters are alphanumeric, `/`, `_`, `-`, `.`, `+`, `:`;
- device address range: `0..63`;
- addresses must be unique within a bus;
- an enabled bus must have at least one address;
- a disabled bus may have an empty address list;
- unknown JSON fields are rejected;
- buses and addresses are serialized in ascending order.

`buses.json` is the single source of truth for configured buses. Generated `/etc/default/mdvwb-N` files are runtime derivatives.

## 7. Driver CLI contract

Primary commands:

```text
MDVWB --help
MDVWB --version
MDVWB --self-test
```

Normal run:

```text
MDVWB --addresses 1,2,3 --port /dev/ttyRS485-1 --bus 1 [options]
```

Legacy positional run remains supported:

```text
MDVWB 1,2,3 /dev/ttyRS485-1 1
```

Required normal-run options:

- `--addresses LIST`;
- `--port NAME`;
- `--bus NUMBER`.

MDV options:

- `--master-id NUMBER`, default `0`, range `0..63`;
- `--period-ms NUMBER`, default `150`;
- `--response-timeout-ms NUMBER`, default `130`.

MQTT options:

- `--mqtt-host HOST`, default `127.0.0.1`;
- `--mqtt-port PORT`, default `1883`;
- `--mqtt-user USER`;
- `--mqtt-password PASSWORD`;
- `--mqtt-client-id ID`, default `mdvwb-<bus>`;
- `--mqtt-keepalive SEC`, default `60`;
- `--mqtt-reconnect SEC`, default `1`;
- `--mqtt-reconnect-max SEC`, default `10`.

Hardware modes:

- `--read-only`: C0 polling only; no MQTT and no write commands;
- `--discover`: scan `0..63`, three complete passes;
- `--test-command NAME=VALUE`: read state, send one command, confirm with C0;
- supported test-command controls: `Power`, `Mode`, `Speed`, `SetTemp`, `Blinds`, `Blok`;
- `--publish-poll-address`: publish `sist-<bus>/GanGetID` each transaction.

`--discover` must not be combined with `--read-only` or `--test-command`.

## 8. Manager CLI contract

```text
mdvwb-manager validate [buses.json]
mdvwb-manager show [buses.json]
mdvwb-manager summary [buses.json]
mdvwb-manager plan [buses.json]
mdvwb-manager apply [buses.json]
mdvwb-manager mqtt [buses.json]
mdvwb-manager migrate-defaults [buses.json]
```

Path resolution:

1. explicit path argument;
2. `MDVWB_BUSES_CONFIG`;
3. `/etc/mdvwb/buses.json`.

Semantics:

- `validate`: parse and validate only;
- `show`: print canonical normalized JSON;
- `summary`: print compact machine-readable bus lines;
- `plan`: print service/config changes without applying them;
- `apply`: atomically write managed `/etc/default/mdvwb-N` files and synchronize services;
- `mqtt`: run the long-lived manager endpoint;
- `migrate-defaults`: build initial JSON from legacy `/etc/default/mdvwb-N` files.

`apply`, `mqtt`, and `migrate-defaults` require root, except tests may set `MDVWB_ALLOW_UNPRIVILEGED_APPLY=1`.

Exit codes:

- `0`: success;
- `1`: manager/runtime error;
- `2`: usage or configuration error.

## 9. Service synchronization rules

Systemd instance name:

```text
mdvwb@<bus>.service
```

Generated environment file:

```text
/etc/default/mdvwb-<bus>
```

Planner actions:

- `WriteConfig`;
- `RemoveConfig`;
- `EnableAndStart`;
- `EnableAndRestart`;
- `DisableAndStop`;
- `EnsureEnabledAndStarted`.

Behavior:

- new enabled bus: write config, enable and start;
- changed enabled bus: write config, enable and restart only that bus;
- unchanged enabled bus: ensure enabled and running;
- disabled bus: disable and stop;
- removed managed bus: disable, stop, remove generated config;
- do not remove unrelated `/etc/default/mdvwb-*` files not recognized as manager-owned;
- writes must remain atomic;
- user-controlled strings must not be passed through an unrestricted shell command.

## 10. Manager MQTT API

Configuration:

```text
/mdvwb/config                 retained current canonical JSON
/mdvwb/config/set             non-retained requested JSON
/mdvwb/config/result          non-retained operation result
/mdvwb/status                 retained manager state
```

Dashboard configuration:

```text
/mdvwb/dashboard/config          retained canonical JSON
/mdvwb/dashboard/config/set      non-retained requested JSON
/mdvwb/dashboard/config/result   non-retained operation result
/mdvwb/dashboard/status          retained backend state
```

Dashboard saves use optimistic concurrency: the submitted revision must equal
the current revision; the manager increments it on success. The file is written
atomically and no bus service is restarted. Missing bus/address references are
reported as warnings through `referenceIssues`, not rejected.

Background upload:

```text
/mdvwb/dashboard/background/upload/start
/mdvwb/dashboard/background/upload/chunk/<uploadId>/<index>
/mdvwb/dashboard/background/upload/finish/<uploadId>
/mdvwb/dashboard/background/upload/cancel/<uploadId>
/mdvwb/dashboard/background/upload/status    retained
/mdvwb/dashboard/background/upload/result    non-retained
```

Uploads are raw binary chunks, maximum 48 KiB each and 10 MiB total. The manager
verifies sequential indexes, SHA-256, real PNG/JPEG/WebP format and dimensions,
then commits a content-addressed `background-*` file. SVG and retained upload
commands are rejected. The dashboard revision is checked again at finish.

Per-bus service control:

```text
/mdvwb/buses/<id>/start
/mdvwb/buses/<id>/stop
/mdvwb/buses/<id>/restart
/mdvwb/buses/<id>/status/get
/mdvwb/buses/<id>/status      retained
/mdvwb/buses/<id>/result      non-retained
```

Discovery:

```text
/mdvwb/buses/<id>/discovery/start
/mdvwb/buses/<id>/discovery/status    retained
/mdvwb/buses/<id>/discovery/result    retained
```

Safety rules:

- all command messages must be non-retained;
- retained commands are ignored;
- configuration payload limit is 64 KiB;
- invalid JSON must not modify the existing file;
- valid JSON is written atomically before service synchronization;
- a synchronization failure may produce `saved=true`, `success=false`;
- start/restart are rejected for `enabled=false` buses;
- stop/status/discovery remain available for configured disabled buses;
- manual start/stop/restart does not edit `buses.json`;
- obsolete bus status/discovery retained topics are cleared;
- device retained topics for addresses removed from configuration are cleared.

## 11. Discovery invariants

- Discovery scans addresses `0..63` sequentially.
- It performs three complete passes by default.
- An address is included after at least one strictly valid C0 response.
- It uses the same serial timing as normal operation.
- The manager stops only the selected `mdvwb@N.service`.
- Other bus processes continue polling.
- The selected service remains stopped after discovery.
- Found addresses are published but never automatically written to `buses.json`.
- The web UI must require the user to edit and save configuration explicitly.

## 12. RS-485 transport invariants

- Serial mode: 4800 baud, 8 data bits, no parity, 1 stop bit.
- All C0/C3/CC/CD transactions share one start-to-start pacer.
- Default start-to-start period: `150 ms`.
- Default response timeout: `130 ms`.
- Each wire request is `0xFE` padding followed by the 16-byte MDV frame.
- Bytes outside a response frame are ignored until `0xAA`.
- After `0xAA`, collect exactly 32 bytes.
- A `0x55` byte inside payload does not terminate collection.
- Do not create separate timing paths for reads, writes, lock, or unlock.

## 13. MDV XYE request protocol

Request length: exactly 16 bytes, indexed `0..15`.

| Byte | Meaning |
|---:|---|
| 0 | `0xAA` frame start |
| 1 | command: C0 read, C3 set, CC lock, CD unlock |
| 2 | device address `0x00..0x3F` |
| 3 | master ID `0x00..0x3F` |
| 4 | always `0x80` |
| 5 | master ID |
| 6 | power + mode |
| 7 | fan speed |
| 8 | setpoint `16..32` |
| 9 | additional functions |
| 10 | timer on, currently `0` |
| 11 | timer off, currently `0` |
| 12 | unknown/reserved, currently `0` |
| 13 | bitwise complement of command byte |
| 14 | checksum |
| 15 | `0x55` frame end |

Checksum invariant:

```text
sum(request bytes 1..14) mod 256 == 0
```

C0, CC, and CD frames use zero payload fields `6..12` unless protocol code explicitly defines otherwise.

### Byte 6: power and mode

- bit 0: Fan;
- bit 1: Dry;
- bit 2: Heat;
- bit 3: Cool;
- bit 4: Auto;
- bit 5: mode lock in responses;
- bit 6: reserved;
- bit 7: Power.

Command invariant:

- exactly one of Fan/Dry/Heat/Cool/Auto must be set in a C3 command;
- Power is independent and may be combined with the selected mode;
- changing mode must preserve Power.

### Byte 7: fan speed

- bit 0: High;
- bit 1: Medium;
- bit 2: Low;
- bit 7: Auto.

Command invariant: exactly one speed value must be set.

### Byte 9: known additional functions

- bit 0: Eco;
- bit 1: electric heater;
- bit 2: blinds/louvers;
- bit 3: fan function;
- reserved bits must not be emitted by the current implementation.

## 14. MDV XYE response protocol

Response length: exactly 32 bytes, indexed `0..31`.

| Byte | Meaning |
|---:|---|
| 0 | `0xAA` |
| 1 | response command C0/C3/CC/CD |
| 2 | always `0x80` |
| 3 | master ID |
| 4 | device address |
| 5 | master ID |
| 6 | unknown |
| 7 | capabilities |
| 8 | power + mode |
| 9 | fan speed |
| 10 | setpoint |
| 11 | room temperature T1 |
| 12 | T2A |
| 13 | T2B |
| 14 | T3 |
| 15 | current/power-consumption field |
| 16 | unknown |
| 17 | timer start |
| 18 | timer stop |
| 19 | unknown |
| 20 | additional functions |
| 21 | status bits |
| 22 | E0..E7 errors |
| 23 | E8..EF errors |
| 24 | P0..P7 protections |
| 25 | P8/PF protections |
| 26 | communication errors 0#..7# |
| 27..29 | unknown |
| 30 | checksum |
| 31 | `0x55` |

Checksum invariant:

```text
sum(response bytes 1..30) mod 256 == 0
```

Temperature conversion:

```text
T = raw / 2.0 - 20.0
```

`0xFF` means room temperature unavailable.

Response decoding rules differ from command encoding:

- Auto plus one active physical mode is valid;
- Auto speed plus one active physical speed is valid;
- several simultaneous non-Auto physical modes are invalid;
- several simultaneous physical speed bits are invalid.

## 15. Device state and command queue rules

- A device must receive a valid C0 before write controls can be safely applied.
- Only verified C0 data synchronizes the cached complete C3 frame.
- C3 responses may contain old values and must not overwrite confirmed state.
- A command modifies only its field in the cached complete C3 frame.
- Power changes preserve mode.
- Mode changes preserve Power.
- Other unchanged fields remain copied from the last verified state.
- After C3/CC/CD, queue a C0 confirmation.
- Immediate responses can still expose the old setpoint; confirmation logic must tolerate this.
- Queue priority is: confirmation read, lock/unlock, set command, ordinary round-robin read.
- One transaction is executed per driver iteration.
- MQTT callbacks only enqueue messages; the RS-485 worker applies them later.
- Do not allow MQTT and serial callbacks to mutate `DeviceContext` concurrently.

`Blok` is exposed separately and must not alter overall `Status` calculation.

## 16. Fan-coil MQTT contract

Device name:

```text
Fan-<bus>_<address>
```

Factual retained states:

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

Commands:

```text
/devices/Fan-<bus>_<address>/controls/<Control>/on1
```

Supported command controls and payloads:

- `Power`: `0` off, `1` on;
- `Mode`: `0` Cool, `1` Heat, `2` Dry, `3` Fan, `4` Auto;
- `Speed`: `1` Low, `2` Medium, `3` High, `4` Auto;
- `SetTemp`: integer `16..32`;
- `Blinds`: `0` or `1`;
- `Blok`: `0` unlock or `1` lock.

Command safety:

- payload must be one integer;
- retained commands are rejected;
- a driver ignores commands for a different bus;
- unknown devices, uninitialized devices, bad values, and unsupported controls are errors;
- command topics are `/on1`; do not change to `/on`.

State publishing:

- publish only verified C0 factual state;
- publish only when changed, except forced initial snapshot;
- publish with MQTT retain enabled;
- never publish C3/CC/CD response payload as factual state;
- offline publishes `Alarm=2`, `Status=7`.

State value mapping:

- `Power`: `0/1`;
- `Mode`: same mapping as commands;
- `Speed`: same mapping as commands;
- `Alarm`: `0` none, `1` device alarm, `2` communication/offline;
- `AlarmCode`: first E0..EF error mapped to `1..16`, `0` none;
- `Status`: `0` off, `1` cooling, `2` heating, `3` dry, `4` fan, `5` auto, `6` alarm, `7` offline;
- `Temp`: numeric room temperature;
- `Blok` does not change `Status`.

## 17. System-device MQTT contract

Device name:

```text
sist-<bus>
```

Topics:

```text
/devices/sist-<bus>/controls/Serial
/devices/sist-<bus>/controls/Error
/devices/sist-<bus>/controls/GanGetID
```

- `Serial` and `Error` are retained and published only on change.
- `GanGetID` is disabled by default to avoid traffic every 150 ms.
- `--publish-poll-address` enables it for diagnostics.
- an ordinary device timeout is represented by device `Alarm=2`, `Status=7`; it should not overwrite system `Error` every poll.

## 18. Wiren Board MQTT metadata

The C++ driver publishes retained metadata for:

```text
/devices/Fan-<bus>_<address>/meta/...
/devices/Fan-<bus>_<address>/controls/<Control>/meta/...
/devices/sist-<bus>/meta/...
```

This replaces the old wb-rules `ArrID` script.

Current fan control order:

1. Alarm
2. AlarmCode
3. Blinds
4. Blok
5. Mode
6. Power
7. SetTemp
8. Speed
9. Status
10. Temp

Current code publishes metadata controls as readonly. Do not silently change metadata semantics without testing the standard Wiren Board Devices UI and `/on1` command workflow.

When a bus/address is removed, stale retained metadata and state must be cleared so obsolete devices disappear.

## 19. Web UI invariants

- Static files only; no build step.
- No internet dependency.
- MQTT WebSocket endpoint is derived from current host and `/mqtt`.
- Page subscribes to manager config/status/service/discovery topics.
- Cards are generated dynamically from current configuration.
- Bus editor uses a local draft.
- Config is published only after explicit save.
- Service/discovery buttons are disabled while draft changes are unsaved.
- Adding, editing, disabling, and deleting arbitrary buses is supported.
- Discovery results are display-only.
- Demo mode remains available through `?demo=1` for local testing.

## 20. Installation and migration invariants

Installers support:

- online build/install through `deploy/install_wirenboard.sh`;
- offline ARM64 install through `deploy/offline-install.sh`.

Default web root is `/var/www` and may be overridden with `MDVWB_WWW_ROOT`.

Upgrade rules:

- preserve existing non-empty `/etc/mdvwb/buses.json`;
- if missing, migrate legacy `/etc/default/mdvwb-N` files;
- if no legacy configuration exists, install example configuration;
- disable/remove obsolete fixed services such as `mdvwb.service` and `mdvwb-2.service`;
- disable the legacy wb-rules file only when it contains both hardcoded `ArrID` and `defineVirtualDevice("Fan-")`;
- keep a `.disabled-mdvwb` backup;
- remove obsolete service-control wb-rules integration;
- clear old retained `Fan-*` and `sist-*` topics where the platform helper is available;
- run version/self-test/validation before final service startup;
- start `mdvwb-manager.service` and apply active bus configuration.

Do not overwrite a working user configuration during an update.

## 21. Build and test commands

Windows/Visual Studio preset workflow:

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
ctest --test-dir out/build/x64-debug -C Debug --output-on-failure
node tests/web/mdvwb_web_model_test.mjs
```

Portable CMake workflow:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

Release builds for real MQTT operation must find libmosquitto. Use:

```text
-DMDVWB_REQUIRE_MOSQUITTO=ON
```

The GitHub workflow `.github/workflows/build-arm64-offline.yml` uses a native ARM runner and produces the offline artifact. Do not switch back to QEMU emulation; it previously failed in Debian package operations.

Required tests currently include:

- `mdv_protocol_self_test`;
- `mdv_buses_config_test`;
- `mdvwb_manager_cli_test`;
- `mdvwb_service_sync_test`;
- `mdvwb_manager_mqtt_test`;
- `mdvwb_discovery_runner_test`;
- `mdvwb_migration_test`;
- Node web model test.

## 22. Change rules for future agents

Before editing:

1. Read this file.
2. Inspect current source; source is authoritative when this file and code differ.
3. Preserve real working behavior unless a change is explicitly requested.
4. Prefer compact changes and minimal new files.
5. Add or update tests for protocol, MQTT, config, migration, or service behavior.
6. Do not assume hardware behavior not supported by captured frames or user testing.

When changing protocol code:

- keep fixed frame sizes;
- keep checksum invariants;
- keep individual addressing;
- keep exactly one command mode and speed;
- preserve valid Auto + physical response combinations;
- do not update confirmed state from C3;
- keep one common 150 ms pacer unless hardware tests justify a deliberate change.

When changing MQTT:

- retain factual states and metadata;
- reject retained commands;
- keep `/on1` command suffix;
- publish only on actual change;
- clear obsolete retained topics during configuration removal;
- preserve current numeric mappings.

When changing multi-bus management:

- support arbitrary bus count;
- keep one process per bus;
- never let one bus operation interrupt another bus;
- validate before writing;
- write configuration atomically;
- avoid shell injection;
- never auto-apply discovery addresses.

When changing deployment:

- use `/var/www/mdvwb` by default;
- preserve `/etc/mdvwb/buses.json`;
- preserve offline installation;
- keep ARM64/Bullseye compatibility;
- do not require internet on the target controller for the offline path.

## 23. Collaboration constraints

The project owner prefers incremental archive-based development on Windows.

For implementation steps:

- provide a ZIP archive;
- use one-line PowerShell commands;
- group commands by unpack, build/run, verification, and Git;
- avoid helper apply scripts and unnecessary preflight checks;
- create a local Git commit after every implementation step and require its hash before continuing;
- do not trigger ARM64 CI until the final integration checkpoint;
- use Git/CI only when an integrated checkpoint or new ARM64 artifact is required;
- keep architecture compact and avoid unnecessary infrastructure.

## 24. Known historical failure modes

Do not repeat these regressions:

- running Git reset outside the repository directory;
- publishing actual state to `/on` or `/on1` instead of the base control topic;
- publishing actual state without retain;
- treating a C3 reply as confirmed new state;
- making Power mutually exclusive with mode;
- rejecting Auto + active mode/speed responses;
- using a fixed two-bus UI or service design;
- placing the web UI in `/mnt/data/www/mdvwb` on the tested controller;
- continuing to create devices through hardcoded wb-rules `ArrID`;
- applying discovery results automatically;
- restarting the discovered bus automatically;
- using QEMU for the ARM64 package workflow.

## 25. Documentation plan

The maintained documentation set should become:

```text
AGENTS.md
README.md
docs/DEVELOPER.md
docs/INSTALLATION.md
docs/WEB_AND_FANCOILS.md
```

`AGENTS.md` is the AI context. Human-facing details belong in the other files. Avoid duplicating large human explanations here; keep this file dense, technical, and authoritative for future coding agents.

## Dashboard editor implementation checkpoint

The `/mdvwb/` engineering UI has bus configuration and dashboard configuration sections. Dashboard browser code is split into `dashboard-editor.js`, `dashboard-model.js`, and `dashboard-placement-editor.js`. The placement editor builds its device catalog from the applied `buses.json`, supports devices from arbitrary buses, stores relative coordinates, and allows one placement per `bus/address`. Missing bus/address references remain editable repairable markers and must never be deleted automatically. The MQTT client supports binary `Uint8Array` payloads for 48 KiB background chunks. Browser SHA-256 must continue to work on plain HTTP; do not remove the local fallback in favor of secure-context-only Web Crypto.

## Dashboard cycle step 7: operational page

- `www/fancoils/` is the separate operational page served at `/fancoils/`.
- It imports the shared browser MQTT client and dashboard model from `www/mdvwb/`; do not duplicate or diverge those contracts.
- It subscribes to `/mdvwb/dashboard/config`, `/mdvwb/dashboard/status` and `/devices/+/controls/+`.
- Individual commands are published non-retained to `/devices/Fan-<bus>_<address>/controls/<Control>/on1`; never optimistically rewrite factual state. A command remains pending until the matching factual base control confirms it or the 10-second UI timeout expires.
- The page must consume factual base control topics only. It must not treat `/on1` command publications as state.
- Marker state colors are derived from factual `Status`; `Alarm=2` or `Status=7` means offline.
- Hidden placements remain in `dashboard.json` but are not rendered on the operational page.
- Step 7 is intentionally read-only. Individual command controls are introduced in the next isolated step.


## Dashboard group-control invariant

The `/fancoils/` group editor may change only explicitly enabled `Power`, `Mode`, `Speed`, and `SetTemp` fields. It sends one non-retained `/on1` command per fan-coil and per selected field. Never replace this with MDV broadcast `0xFF`, retained commands, or optimistic state updates. Offline/unknown devices are skipped and the UI waits for factual base-topic confirmation for each operation.


## Compact operational UI invariant

The user-facing `/fancoils/` page must keep one compact header and devote the remaining viewport height to the plan. Individual details, group commands and future schedules open as right-side drawers; they must not create additional full-width header sections. The page must not link to `/mdvwb/`, expose editor functions, or display/control `Blinds` and `Blok`. Normal text selection is disabled only in `/fancoils/`; do not apply that restriction to the engineering `/mdvwb/` UI.

<!-- Step 9.2 UI correction: wheel zoom, compact numbered markers, map remains interactive while side drawers are open, direct individual/group selection, no status filter in header. -->

<!-- Step 9.3 UI invariant: the operational map is centered with pan margins, supports pointer dragging and wheel zoom, marker numbers must remain readable through 200, pending styling must not reflow control rows, and hover tooltips show label, mode, temperature and speed. -->


### Compact dashboard editor correction

The engineering dashboard editor is now a single-header, map-first workspace. General settings and background upload share the temporary Parameters drawer, the search field is removed, markers use the approved fan icon, and every placement has a unique editable user number from 1 to 200. Existing configurations without a number are migrated sequentially.

## Administrative web layout

- Keep `/mdvwb/#buses` to one compact top bar. Do not reintroduce stacked summary, manager, configuration, and section headers.
- The dashboard editor must derive bus groups from `/mdvwb/config`; never assume exactly two buses.
- Device and bus checkboxes control whether fan coils are visible on the current map.
- The user number is visible in the left catalog and on the map, remains unique in the range 1–200, and is independent from MQTT bus/address.
- Editor markers must visually match the numbered circular markers used by `/fancoils/`.
- Coordinates snap to a 1% X/Y grid.

- Dashboard editor marker size is one panel-wide UI setting. Keep all persisted `fans[].markerScale` values equal; do not reintroduce per-fan size controls.
- Selecting a dashboard fan must highlight it both in the map and in the left device catalog. A click must never change coordinates; only an actual pointer drag may move a marker.
- Bus administration cards are full-width vertical rows, not an auto-fitting multi-column grid.


### Multiple dashboard panels

- `/etc/mdvwb/dashboard.json` canonical format is version 2: one global `revision`, `defaultPanel`, and 1–64 panel objects.
- Continue accepting legacy version 1 and migrate it to panel `main`; never require a manual conversion before startup.
- Panel IDs are stable URL identifiers `[A-Za-z0-9_-]`, 1–48 characters.
- The operator URL is `/fancoils/?panel=<id>`. The operator page must not expose a panel selector or an admin link.
- Each panel owns its title, image, fan list, user numbers and coordinates. Numbers are unique per panel, not globally.
- Dashboard saves remain complete collection transactions protected by the global revision.
- Background upload start must contain `panelId`; reject unknown panel IDs and update only the selected panel.
- Image upload belongs inside Parameters. Do not restore a separate permanent image block or header button.


### Step 9.8 editor interaction correction

The dashboard editor keeps the 1% grid switch inside **Panel settings**. The editor header contains no zoom controls; wheel input over the map changes only the temporary editor preview scale. The saved opening scale remains the explicit setting in the drawer. Marker rotation is no longer exposed or rendered; legacy `rotation` values are accepted for compatibility and normalized to zero on the next save.


## Schedule backend invariant

- `/etc/mdvwb/schedules.json` is strict schema version 1 with one global optimistic-concurrency revision.
- Schedule targets are always individual `bus/address` pairs; never use protocol broadcast.
- Every saved target must exist in `buses.json` and be visible in the referenced dashboard panel.
- Actions are opt-in Power/Mode/Speed/SetTemp fields and at least one action is required.
- Manager validates `/run` and emits `/execute`; actual timing and `/on1` publishing belong to the separate scheduler service.
- Retained save/run commands are forbidden. Configuration and status are retained; operation events/results are non-retained.


## Scheduler execution invariant

- `mdvwb-scheduler` is the only component that converts schedule time or `/execute` events into fan `/on1` commands.
- Use controller local time; weekly days are Monday=1 through Sunday=7.
- Persist automatic execution minute before queuing commands so restart in the same minute cannot duplicate a run.
- Manual execution remains allowed when `enabled=false`; automatic execution does not.
- Publish Mode, Speed, SetTemp, then Power, each non-retained and individually addressed.
- A run completes only after matching factual base topics confirm every requested value; default timeout is 10 seconds.
- Never use broadcast, retained commands, C3 replies, or optimistic state as confirmation.
