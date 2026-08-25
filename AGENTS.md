# MDVWB repository guide for coding agents

This file is a compact map of the current repository and its non-negotiable architectural rules. It is not end-user documentation.

Current CMake project version: **1.3.0**.

## 1. Source-of-truth order

When information conflicts, use this order:

1. current source code;
2. current automated tests;
3. `CMakeLists.txt`, CMake presets, systemd units, deployment scripts, and CI workflows;
4. current JSON examples and environment templates;
5. Markdown documentation.

Never preserve behavior solely because an old document describes it. Verify it in the current implementation and tests first.

## 2. Project identity

- Language: C++20.
- Primary target: Wiren Board ARM64.
- Release build environment: Debian 11 Bullseye on native ARM64.
- Runtime dependencies: systemd, Mosquitto, and `libmosquitto.so.1`.
- Development host used by the maintainer: Windows with Visual Studio CMake and PowerShell.
- Repository: `Lex26p/MDVWB`.
- Typical local path: `C:\Projects\MDVWB`.
- The repository and runtime are separate from `wb-mqtt-serial`, but the product
  is a Wiren Board extension that depends on its Mosquitto, systemd and web
  environment. Do not describe it as a standalone automation platform.

The old C# implementation may be used only as historical protocol evidence. Do not port its architecture back into this project.

## 3. Executables and process boundaries

CMake builds five executables; `mdvwb-modbus` is installed as an internal runtime:

| Target | Responsibility |
|---|---|
| `MDVWB` | One MDV XYE RS-485 bus driver process |
| `mdvwb-modbus` | One profile-driven Modbus RTU bus driver process |
| `mdvwb-offline` | Retained offline-state publisher used by `ExecStopPost` |
| `mdvwb-manager` | Configuration, systemd, discovery, dashboard upload, and management MQTT API |
| `mdvwb-scheduler` | Automatic and manual schedule execution with factual confirmation |

Runtime services:

```text
mdvwb-manager.service
mdvwb-scheduler.service
mdvwb@<bus>.service
```

The architecture requires:

- exactly one protocol-specific bus process per physical serial port;
- arbitrary bus count;
- independent polling and failure isolation between buses;
- manager and scheduler as separate long-lived processes;
- systemd, not application threads, as the owner of bus-process restart policy.

Do not introduce a shared process that opens several serial ports.

## 4. Runtime topology

```text
www/mdvwb/  ─┐
              ├─ MQTT WebSocket /mqtt ─ Mosquitto
www/fancoils/ ┘                         ├─ mdvwb-manager.service
                                       ├─ mdvwb-scheduler.service
                                       ├─ mdvwb@1.service ─ mdvwb-run ─ bus 1 port
                                       ├─ mdvwb@2.service ─ mdvwb-run ─ bus 2 port
                                       └─ mdvwb@N.service ─ mdvwb-run ─ bus N port
```

The manager owns configuration and lifecycle operations. The scheduler reads the same configuration files but does not own them. A bus process owns only its generated `/etc/default/mdvwb-<bus>` runtime configuration and serial port.

## 5. Runtime paths

### Binaries and helper

```text
/usr/local/bin/MDVWB
/usr/local/bin/mdvwb-offline
/usr/local/bin/mdvwb-manager
/usr/local/bin/mdvwb-scheduler
/usr/local/lib/mdvwb/mdvwb-run
/usr/local/lib/mdvwb/mdvwb-modbus
/usr/local/lib/mdvwb/mdvwb.env
/usr/local/lib/mdvwb/modbus-profiles/
```

### Persistent configuration and state

```text
/etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
/etc/mdvwb/schedules.json
/etc/default/mdvwb-manager
/etc/default/mdvwb-scheduler
/etc/default/mdvwb-<bus>
/var/lib/mdvwb/scheduler-state.tsv
```

### systemd

```text
/etc/systemd/system/mdvwb@.service
/etc/systemd/system/mdvwb-manager.service
/etc/systemd/system/mdvwb-scheduler.service
```

### Static web and assets

```text
/var/www/mdvwb/
/var/www/fancoils/
/var/www/fancoils/assets/
```

The default web root is `/var/www`. Do not restore obsolete `/mnt/data/www/...` paths.

## 6. Repository layout

### `src/driver/`

| Files | Responsibility |
|---|---|
| `MDVWB.cpp`, `MDVWB.h` | Entry point, run modes, self-test orchestration |
| `mdv_config.*` | Driver CLI and option validation |
| `mdv_protocol.*` | Request construction, checksum, response validation and decoding |
| `mdv_serial.*` | Platform serial transport, wire request, pacing and transaction timeout |
| `mdv_device.*` | Per-device confirmed state, cached complete C3 frame, desired revisions and pending fields |
| `mdv_driver.*` | Polling, command scheduling, confirmation reads and communication state |
| `mdv_discovery.*` | Three-pass scan of addresses `0..63` |
| `mdv_mqtt.*` | Fan-coil MQTT parsing, bounded command intake, factual state and system-device publications |
| `mdv_metadata.*` | Wiren Board retained metadata |
| `mdv_mosquitto.*` | Shared asynchronous libmosquitto transport |
| `mdv_offline.cpp` | Offline publisher executable |
| `mdv_bounded_queue.h` | Shared bounded latest-value queue used by MQTT-facing components |
| `modbus_rtu.*`, `modbus_rtu_serial.*` | Modbus RTU framing, validation and serial transport |
| `modbus_profile.*`, `modbus_runtime_profile.*` | Strict schema-v1 profile loading and runtime capability validation |
| `modbus_value.*`, `modbus_semantic.*`, `modbus_resolver.*` | Profile-driven value conversion, semantic mapping and logical-address resolution |
| `modbus_scan.*`, `modbus_scan_execute.*` | Safe read-only logical-address scan |
| `modbus_poll_plan.*`, `modbus_driver.*` | Resolved polling plans, batching, factual state and confirmed writes |
| `modbus_runtime_config.*`, `modbus_runtime_main.cpp` | Managed environment validation and internal Modbus runtime entry point |

### `src/manager/`

| Files | Responsibility |
|---|---|
| `mdv_buses_config.*` | Strict buses JSON parsing, validation and canonical serialization |
| `mdv_dashboard_config.*` | Dashboard schema, canonicalization and bus/address reference inspection |
| `mdv_schedules_config.*` | Schedule schema, validation and reference inspection |
| `mdvwb_dashboard_upload.*` | Sequential image chunks, SHA-256, format detection and temporary assets |
| `mdvwb_service_sync.*` | Generated environment files and systemd synchronization plan/apply |
| `mdvwb_discovery_runner.*` | Runs `MDVWB --discover` and parses its output |
| `mdv_modbus_discovery.*` | Runs validated profile-driven Modbus discovery |
| `mdvwb_manager_mqtt.*` | Long-lived management MQTT service, bounded intake, revisions and transactions |
| `mdvwb_migration.*` | Strict legacy environment-file migration |
| `mdvwb_manager_cli.*` | Manager CLI dispatch and privilege checks |
| `mdvwb_manager_main.cpp` | `mdvwb-manager` entry point |

### `src/scheduler/`

| Files | Responsibility |
|---|---|
| `mdvwb_scheduler.*` | Schedule selection, execution, confirmation, queues and configuration freshness |
| `mdvwb_scheduler_main.cpp` | Scheduler environment parsing and daemon entry point |

### `deploy/`

Important files:

```text
mdvwb@.service
mdvwb-manager.service
mdvwb-scheduler.service
mdvwb-run
mdvwb.env
mdvwb-manager.env
mdvwb-scheduler.env
buses.example.json
dashboard.default.json
schedules.default.json
install_wirenboard.sh
offline-install.sh
```

### `www/`

Engineering application:

```text
www/mdvwb/index.html
www/mdvwb/app.js
www/mdvwb/model.js
www/mdvwb/mqtt-client.js
www/mdvwb/dashboard-editor.js
www/mdvwb/dashboard-model.js
www/mdvwb/dashboard-placement-editor.js
www/mdvwb/styles.css
```

Operator application:

```text
www/fancoils/index.html
www/fancoils/app.js
www/fancoils/model.js
www/fancoils/schedule-model.js
www/fancoils/scheduler-status-ui.js
www/fancoils/scheduler-status-health.js
www/fancoils/styles.css
```

Both applications are static and have no production build step or external browser dependency.

Current integration caveat: `scheduler-status-ui.js` imports `scheduler-status-health.js`, but `/fancoils/index.html` currently loads only `app.js`, and `app.js` does not import the status UI module. Treat the two scheduler-status files as dormant helpers, not as active production-page behavior. A future integration must update the browser entry point, package required-file checks, both workflows and integration tests together.

### Workflows

```text
.github/workflows/validate.yml
.github/workflows/build-arm64-offline.yml
```

`validate.yml` performs a Release build with required Mosquitto support, runs the complete CTest suite and all `tests/web/*.mjs` model tests. The ARM64 workflow runs manually on a native ARM64 runner, builds inside Debian 11 Bullseye, and creates the offline artifact.

## 7. CMake target map

Production executables:

```text
MDVWB
mdvwb-modbus
mdvwb-offline
mdvwb-manager
mdvwb-scheduler
```

Libraries:

```text
mdvwb_json
mdvwb_mosquitto_transport
mdvwb_serial_port
mdvwb_modbus_rtu
mdvwb_modbus_profile
mdvwb_modbus_scan
mdvwb_modbus_poll_plan
mdvwb_modbus_driver
mdvwb_modbus_runtime_cadence
mdvwb_modbus_runtime_config
mdvwb_buses_config
mdvwb_modbus_bus_config
mdvwb_modbus_profile_ui
mdvwb_modbus_profile_mqtt
mdvwb_dashboard_config
mdvwb_schedules_config
mdvwb_dashboard_upload
mdvwb_service_sync
mdvwb_modbus_discovery
mdvwb_discovery_runner
mdvwb_manager_mqtt
mdvwb_manager_cli
mdvwb_scheduler
```

All targets compile as C++20. MSVC uses `/W4 /permissive- /utf-8`; other compilers use `-Wall -Wextra -Wpedantic`.

`MDVWB_REQUIRE_MOSQUITTO=ON` is mandatory for production and release-package builds. Keeping Mosquitto optional is only for local protocol/configuration development.

## 8. CTest ownership

CMake currently registers 43 tests:

| Test | Primary ownership |
|---|---|
| `mdvwb_json_test` | Strict shared JSON parser behavior |
| `mdv_protocol_self_test` | Frames, parser, serial pacing, driver core and built-in invariants |
| `mdvwb_offline_publisher_test` | Offline-publisher argument and payload behavior |
| `mdvwb_mqtt_delivery_test` | Driver MQTT transport/delivery behavior |
| `mdvwb_driver_fairness_test` | Command retry, polling fairness and confirmation behavior |
| `mdvwb_modbus_rtu_test`, `mdvwb_modbus_rtu_serial_test` | RTU frames, exceptions, CRC, timing and serial transport |
| `mdvwb_modbus_profile_test`, `mdvwb_modbus_value_test`, `mdvwb_modbus_semantic_test` | Strict profiles, numeric conversion and semantic capability mapping |
| `mdvwb_modbus_resolver_test`, `mdvwb_modbus_scan_test`, `mdvwb_modbus_scan_execute_test` | Logical addressing and safe discovery probes |
| `mdvwb_modbus_poll_plan_test`, `mdvwb_modbus_driver_test`, `mdvwb_modbus_runtime_cadence_test` | Poll plans, driver behavior, retry and cadence policy |
| `mdvwb_modbus_vrf_reference_test`, `mdvwb_modbus_vrf_profile_test` | First production profile and preserved equipment facts |
| `mdvwb_modbus_thermostat_profile_test` | Direct-Slave Thermostat profile, mappings, FC06 writes and FC03 confirmation |
| `mdvwb_modbus_runtime_config_test`, `mdvwb_modbus_mqtt_integration_test` | Managed runtime validation and end-to-end MQTT semantics |
| `mdv_buses_config_test` | Bus schema and canonicalization |
| `mdvwb_modbus_buses_config_test`, `mdvwb_modbus_bus_profile_test` | Protocol-aware bus configuration and profile compatibility |
| `mdvwb_modbus_profile_ui_test`, `mdvwb_modbus_profile_mqtt_test` | Retained profile catalog and browser-facing capabilities |
| `mdvwb_dashboard_config_test` | Dashboard schemas and references |
| `mdvwb_schedules_config_test` | Schedule schemas and references |
| `mdvwb_dashboard_upload_test` | SHA-256, image formats and sequential chunks |
| `mdvwb_manager_cli_test` | CLI, paths, privilege checks and output |
| `mdvwb_service_sync_test` | Environment rendering and systemd plans |
| `mdvwb_modbus_service_sync_test` | Modbus runtime environment and service selection |
| `mdvwb_manager_mqtt_test` | Management MQTT API and runtime operations |
| `mdvwb_manager_revision_test` | Configuration revision conflict behavior |
| `mdvwb_dashboard_concurrency_test` | Dashboard save/upload revision concurrency |
| `mdvwb_manager_transaction_test` | Save/apply transactional behavior and rollback |
| `mdvwb_modbus_discovery_test` | Validated profile-driven Modbus discovery |
| `mdvwb_discovery_runner_test` | Discovery process invocation and output parsing |
| `mdvwb_discovery_async_test` | Same-bus exclusion and different-bus parallel discovery |
| `mdvwb_migration_test` | Strict legacy migration and ambiguous-input rejection |
| `mdvwb_scheduler_test` | Schedule execution and factual confirmation |
| `mdvwb_scheduler_freshness_test` | Runtime dependency changes and stale-reference blocking |
| `mdvwb_mqtt_command_delivery_test` | Shared manager/scheduler queue delivery behavior |

Seven JavaScript model tests under `tests/web/` are additional checks, run by `validate.yml`, and are not registered in CTest.

Run the complete local Windows suite with:

```powershell
cmake --preset x64-debug
cmake --build out/build/x64-debug
ctest --test-dir out/build/x64-debug -C Debug --output-on-failure
```

## 9. Configuration ownership

### `buses.json`

- schema version `1`;
- manager is the writer and source-of-truth owner;
- canonical manager output includes `revision`;
- missing `protocol` is accepted as legacy `mdv`, while canonical output writes it explicitly;
- `pollPeriodMs` is configured per bus; missing values default to `150` for
  MDV and `300` for Modbus RTU, while canonical output writes the effective value;
- `modbus_rtu` buses require validated profile and serial settings and logical addresses `1..63`;
- generated `/etc/default/mdvwb-<bus>` files are derivatives;
- changes may start, restart, stop, or remove only affected service instances;
- removed bus/device retained topics must be cleared.

### `dashboard.json`

- current collection schema version `2`;
- manager creates a default when missing;
- supports multiple independent panels and `defaultPanel`;
- browser saves use optimistic revision checks;
- background upload finish checks the revision again;
- successful or failed operation results report the current authoritative revision;
- dashboard changes do not restart RS-485 services.

### `schedules.json`

- scheduler reads but does not own the file;
- manager owns validation and MQTT saves;
- execution validates schedule, bus, dashboard and device references;
- before publishing, execution rejects any Modbus action not backed by both an
  enabled writable profile point and the current runtime write implementation;
- scheduler detects content changes using fingerprints, not only timestamp/size;
- invalid current dependencies block stale execution until repaired.

### `scheduler-state.tsv`

This file prevents an automatic schedule from executing twice after a scheduler restart in the same local minute. It is state, not user configuration.

## 10. Non-negotiable MDV driver invariants

- request frame: exactly 16 bytes from `0xAA` to `0x55`;
- response frame: exactly 32 bytes from `0xAA` to `0x55`;
- wire request has an additional leading `0xFE` byte;
- bytes outside a response are ignored until `0xAA`;
- a payload `0x55` does not terminate response collection early;
- all C0/C3/CC/CD transactions share one start-to-start pacer;
- the managed per-bus period defaults to 150 ms, is configurable in the
  `150..60000 ms` range, and the response timeout remains 130 ms;
- command address is individual `0..63`; broadcast `0xFF` is not used;
- Power is independent from Mode;
- an outgoing C3 has exactly one Mode and one Speed selection;
- responses may represent Auto plus a physical active mode/speed;
- a valid C0 is the only source of confirmed factual state;
- C3/CC/CD replies cannot synchronize the factual cache;
- a command cannot be built before the first valid C0 initializes the device;
- local panel changes may update non-pending cached fields;
- an old C0 must not overwrite a newer still-pending desired field;
- factual MQTT topics are retained base topics;
- command MQTT topics are non-retained and end in `/on1`.

### Modbus runtime invariants

- Modbus RTU uses logical addresses `1..63`; a logical address is not necessarily a Slave ID;
- all manufacturer register knowledge comes from a selected schema-v1 profile;
- effective write capability is the intersection of the profile declaration and
  the current runtime implementation; the production runtime currently performs
  confirmed writes for `Power`, `Mode`, `FanSpeed` and `SetTemperature`;
- scalar Holding Register writes use the function declared by each write point:
  FC10 by default or explicit FC06 `write_single_register`; both require factual
  FC03 read-back;
- a valid profile may declare other future writes without making them
  available to the UI or scheduler;
- profiles and managed runtime settings are strictly validated before serial I/O or discovery;
- integer profile fields must be range-checked before narrowing to fixed-width types;
- discovery is read-only and uses only the selected profile's safe probe;
- a write response alone is not factual state; matching profile-driven read-back is required;
- an already-online device becomes offline only after three consecutive failed
  complete polling attempts; one complete successful poll resets the counter
  and restores online state;
- the managed per-bus Modbus logical-device operation period defaults to
  300 ms, is configurable in the `150..60000 ms` range, and is a lower bound
  for ordinary polls, writes, confirmations and retry scheduling;
- write and confirmation failures do not change availability by themselves;
  runtime failure diagnostics identify the logical address, stage, Slave ID
  and register or register range;
- the shipped `vrf_add_controller` currently enables table-derived Mode,
  FanSpeed and integer SetTemperature mappings for field validation; these
  mappings are not yet recorded as live-hardware-confirmed;
- that profile uses only SetTemperature integer registers `40031/40081` and
  whole-degree commands `16..32`; half-degree registers `40037/40085` remain
  unsupported;
- that profile exposes read-only integer RoomTemperature from the existing
  presence-probe register `40039 + 91*Y`; polling must reuse the validated
  probe response instead of adding a second read of that register;
- the shipped `thermostat` profile uses direct logical-address-to-Slave-ID
  mapping, `9600 8N1`, literal PDU addresses `0x2060..0x2065`, FC03 reads and
  explicit FC06 writes;
- `thermostat` exposes Power `1/2`, Cool/Heat `1/2`, FanSpeed
  Low/Medium/High/Auto `1/2/3/4`, RoomTemperature from raw-tenths register
  `0x2063`, and
  whole-degree SetTemperature `16..34` through raw-tenths register `0x2065`;
- the second spreadsheet block, advanced settings, `0x2064` and `0x2066` are
  intentionally not exposed by `thermostat`; its live hardware behavior is not
  yet recorded as confirmed;
- unsupported controls produce no Modbus wire traffic;
- when an optional factual value becomes unavailable, its old retained MQTT value is cleared with an empty retained payload;
- for compatibility, an online powered device without a factual Mode still maps to `Status=5` (Auto); this does not create a factual `Mode` value.

## 11. Offline-state invariant

`mdvwb@.service` uses:

```text
ExecStopPost=-/usr/local/lib/mdvwb/mdvwb-run --publish-offline
```

The helper invokes `mdvwb-offline` with the configured bus, addresses and MQTT settings. It runs after normal stop and unexpected exit. Do not replace this with a manager-only callback that would fail when the manager is unavailable.

## 12. Queue and threading invariants

MQTT callbacks enqueue parsed work; they must not mutate serial-device state directly.

The driver, manager and scheduler use bounded queues with byte limits. Same-key newer work may replace older queued work. Do not replace these structures with unbounded `std::queue`/`std::deque` intake.

The driver serial loop remains sequential. Manager discovery is the intentional exception: one worker is allowed per bus ID, different buses may run concurrently, and a second discovery for the same bus is rejected.

## 13. Discovery invariants

- MDV scans addresses `0..63` in ascending order for three passes; one strictly valid C0 reply is enough to include an address;
- Modbus scans logical addresses `1..63` with the selected validated profile and read-only probe;
- stop only the selected `mdvwb@N.service`;
- leave that service stopped after completion;
- never apply discovered addresses automatically;
- serialize discovery for the same bus;
- allow independent discovery for different buses;
- keep MQTT manager processing responsive while discovery runs.
- reject start/restart and runtime-changing configuration for a bus while its discovery owns the service;
- a save that changes only unrelated buses must not start or restart the discovery-owned service, including rollback.

## 14. Legacy migration invariants

Migration reads assignment files as data and must never execute them.

Candidates are `/etc/default/mdvwb` and canonical `/etc/default/mdvwb-N` files. Migration must fail on malformed assignments, bad quoting, duplicate `MDVWB_*` variables, missing required fields, non-canonical file names, bus-ID disagreement, duplicate bus sources, invalid ports, invalid/duplicate addresses, or any final schema conflict.

Do not restore the old behavior that silently skipped incomplete files or preferred one conflicting source.

## 15. Dashboard concurrency invariants

Dashboard configuration and background upload share one optimistic revision sequence.

- submitted revision must match the current file;
- successful save increments the revision;
- upload start records the expected revision;
- upload finish reloads and rechecks the current revision;
- an older upload cannot overwrite a concurrent save;
- terminal upload results report the authoritative current revision, including zero;
- conflict handling republishes the current retained dashboard after the result;
- bus services are not restarted by dashboard saves or image uploads.

## 16. Scheduler freshness invariants

Before execution, the scheduler must use current contents of:

```text
/etc/mdvwb/schedules.json
/etc/mdvwb/buses.json
/etc/mdvwb/dashboard.json
```

Fingerprints are required because replacement content can preserve file timestamp and size.

A removed device, removed panel, removed bus, or disabled bus must not be controlled from stale cached configuration. Active and queued runs must reach a clear terminal state when dependencies become invalid. Valid repair must unblock future execution without restarting the scheduler process.

## 17. Deployment invariants

The offline installer:

- requires root and architecture `arm64`;
- requires `libmosquitto.so.1`;
- emits package format `2`; format `1` remains accepted for legacy archives,
  while format `2` requires the internal Modbus runtime and both shipped
  production profiles;
- validates all required package files and `SHA256SUMS`;
- installs all five executables (including internal `mdvwb-modbus`) and three systemd unit types;
- installs both static web applications;
- preserves existing non-empty `buses.json`, `dashboard.json`, and `schedules.json`;
- preserves uploaded files under `/var/www/fancoils/assets`;
- disables obsolete fixed services and the legacy ArrID wb-rules device definition;
- runs driver and offline-publisher self-tests;
- validates and applies bus configuration;
- enables and starts manager and scheduler.

When `buses.json` is absent or empty, both installers run `migrate-defaults`.
Exit code `3` means that no legacy files exist and installs the empty default;
every other migration error aborts installation instead of silently falling back.

The native ARM64 workflow packages the release inside Debian 11 Bullseye and includes internal checksums.

The offline installer and both workflows explicitly require
`scheduler-status-ui.js` and `scheduler-status-health.js`. They remain dormant
helpers until the `/fancoils/` browser entry point imports the status UI module.

## 18. Forbidden regressions

Do not reintroduce:

- fixed support for only one or two buses;
- one multi-port driver process;
- protocol broadcast control;
- `/on` instead of `/on1`;
- retained commands;
- non-retained factual state;
- C3 response as factual state;
- unbounded MQTT queues;
- automatic discovery result application;
- automatic restart of the discovered bus;
- global discovery exclusion across unrelated buses;
- silent/partial legacy migration;
- dashboard writes that ignore revision;
- scheduler execution from stale bus/dashboard references;
- documentation or tests that describe dormant scheduler-status helpers as already wired into `/fancoils/`;
- removal of user JSON or uploaded assets during an update;
- the old `ArrID` virtual-device rule;
- obsolete `/mnt/data/www/mdvwb` web root.

## 19. Change procedure by subsystem

### Driver/protocol change

1. Update exact frame/parser/device/driver tests.
2. Preserve fixed frame lengths and checksum rules.
3. Test first-C0 initialization, old-value confirmation behavior, and neighboring field preservation.
4. Test MQTT publication only from factual C0.
5. Run `mdv_protocol_self_test`, driver fairness, MQTT delivery, then full CTest.

### Manager/configuration change

1. Update parser and canonical serializer tests.
2. Test unknown, missing, duplicate, invalid and revision-conflict input.
3. Test atomic write, systemd plan, apply failure, rollback, and retained publication order.
4. Check obsolete-topic cleanup.
5. Run manager/config/revision/transaction tests, then full CTest.

### Dashboard/upload change

1. Test both schema versions and canonical version-2 output.
2. Test panel and device references.
3. Test upload chunk order, maximum size, SHA, real format and dimensions.
4. Test concurrent save/upload revision conflict.
5. Run dashboard config/upload/concurrency tests, then full CTest.

### Discovery change

1. Test runner invocation/output parsing.
2. Test same-bus rejection and different-bus parallel execution.
3. Verify selected service remains stopped.
4. Verify manager MQTT remains responsive.
5. Run discovery runner/async tests, then full CTest.

### Scheduler change

1. Test schedule schema and references.
2. Test factual confirmation and timeout.
3. Test manual and automatic execution.
4. Test all three configuration fingerprints and invalid-reference blocking.
5. Run scheduler, freshness and integration delivery tests, then full CTest.

### Deployment change

1. Validate shell syntax.
2. Validate default JSON.
3. Check package required-file list and checksums.
4. Confirm preserved files and assets.
5. Run `Validate MDVWB` and the native ARM64 package workflow before release.

## 20. Documentation ownership

- `README.md`: concise current product and repository overview.
- `AGENTS.md`: source map, non-negotiable implementation invariants, and test ownership.
- `docs/DEVELOPER.md`: detailed internal architecture and contracts.
- `docs/INSTALLATION.md`: installation, update, operation, recovery, and diagnostics.
- `docs/WEB_AND_FANCOILS.md`: current engineering and operator UI behavior.
- `docs/schedules-config.md`: canonical schedule schema and MQTT API.
- `docs/RELEASE_CHECKLIST.md`: final release and controller smoke verification.

When changing behavior, update the owning detailed document and only the relevant summary in `README.md`/`AGENTS.md`. Avoid appending chronological “step” notes to permanent documentation.
