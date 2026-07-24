# MDVWB

Standalone C++20 driver for individual control and monitoring of MDV XYE fan-coils through RS-485 and MQTT on Wiren Board.

Current documented version: **1.2.0**.

## Features

- MDV XYE polling and control through RS-485;
- individual addresses `0..63`;
- arbitrary number of independent RS-485 buses;
- one isolated `MDVWB` process per bus;
- retained MQTT state publishing;
- command topics with the `/on1` suffix;
- automatic Wiren Board device metadata;
- shared JSON bus configuration;
- independent revisioned dashboard configuration over MQTT;
- systemd service synchronization through `mdvwb-manager`;
- independent `mdvwb-scheduler` execution of weekly and one-time schedules;
- per-bus start, stop, restart and status;
- per-bus device discovery;
- static offline web configuration interface;
- visual plan editor with multi-bus fan-coil placement;
- migration from legacy per-bus configuration and `ArrID` wb-rules;
- offline ARM64 installation package.

## Runtime architecture

```text
Browser: /var/www/mdvwb
        |
        | MQTT WebSocket /mqtt
        v
Mosquitto
        |
        +-- mdvwb-manager.service
        |      |
        |      +-- /etc/mdvwb/buses.json
        |      +-- /etc/mdvwb/dashboard.json
        |      +-- /etc/mdvwb/schedules.json
        |      +-- /etc/default/mdvwb-N
        |      +-- mdvwb@N.service control
        |      `-- discovery for a selected bus
        |
        +-- mdvwb-scheduler.service
        |      `-- schedule time + /on1 commands + factual confirmation
        |
        +-- mdvwb@1.service --> MDVWB --> RS-485 bus 1
        +-- mdvwb@2.service --> MDVWB --> RS-485 bus 2
        `-- mdvwb@N.service --> MDVWB --> RS-485 bus N
```

Each driver process owns exactly one serial port. Buses run independently and poll simultaneously.

## Bus configuration

Canonical bus configuration:

```text
/etc/mdvwb/buses.json
```

Example:

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
    }
  ]
}
```

Supported constraints:

- bus ID: `1..999`;
- fan-coil address: `0..63`;
- unique bus IDs;
- unique serial ports;
- unique addresses within a bus;
- an enabled bus must contain at least one address.

`buses.json` is the single source of truth. Files `/etc/default/mdvwb-N` are generated runtime configuration.


Dashboard configuration is stored independently:

```text
/etc/mdvwb/dashboard.json
```

The manager creates it on first start and exposes retained MQTT configuration
with optimistic revision protection. Saving the dashboard does not restart bus
processes.

Schedule configuration is stored in:

```text
/etc/mdvwb/schedules.json
```

`mdvwb-scheduler.service` executes enabled weekly and one-time schedules using
the controller local time. It publishes individual non-retained `/on1` commands
and waits up to 10 seconds for factual base-topic confirmation. Automatic
execution state is persisted in `/var/lib/mdvwb/scheduler-state.tsv`, preventing
a restart in the same minute from running the same schedule twice.

## Fan-coil MQTT contract

Device name:

```text
Fan-<bus>_<address>
```

Example:

```text
Fan-1_3
```

Factual states are published retained to base control topics:

```text
/devices/Fan-1_3/controls/Power
/devices/Fan-1_3/controls/Mode
/devices/Fan-1_3/controls/Speed
/devices/Fan-1_3/controls/SetTemp
/devices/Fan-1_3/controls/Temp
/devices/Fan-1_3/controls/Blinds
/devices/Fan-1_3/controls/Blok
/devices/Fan-1_3/controls/Alarm
/devices/Fan-1_3/controls/AlarmCode
/devices/Fan-1_3/controls/Status
```

Commands are accepted only through non-retained `/on1` topics:

```text
/devices/Fan-1_3/controls/Power/on1
/devices/Fan-1_3/controls/Mode/on1
/devices/Fan-1_3/controls/Speed/on1
/devices/Fan-1_3/controls/SetTemp/on1
/devices/Fan-1_3/controls/Blinds/on1
/devices/Fan-1_3/controls/Blok/on1
```

The driver publishes only verified C0 data as factual state. C3/CC/CD replies are not treated as confirmed state.

## Web interface

Installed path:

```text
/var/www/mdvwb
```

Open:

```text
http://<Wiren-Board-address>/mdvwb/
```

The page provides:

- dynamic cards for all configured buses;
- bus creation and editing;
- serial-port and address configuration;
- enable/disable configuration;
- start, stop, restart and status;
- device discovery;
- background upload and scaling;
- fan-coil marker placement with labels, relative coordinates, size, rotation and visibility.

Discovery results are displayed but are not automatically written to the configuration. The selected bus remains stopped after discovery.

The dashboard backend also accepts a background image in sequential binary MQTT
chunks. PNG, JPEG and WebP files up to 10 MiB are verified by SHA-256 and actual
image headers before `dashboard.json` is updated. The upload does not restart
RS-485 services.

## Build and tests

Windows with the Visual Studio CMake preset:

```powershell
cmake --preset x64-debug
cmake --build "out/build/x64-debug"
ctest --test-dir "out/build/x64-debug" -C Debug --output-on-failure
```

Optional web-model test for development machines with Node.js:

```powershell
node ".\tests\web\mdvwb_web_model_test.mjs"
```

Portable CMake:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

A production build must require libmosquitto:

```text
-DMDVWB_REQUIRE_MOSQUITTO=ON
```

## Offline installation

GitHub Actions produces:

```text
MDVWB-arm64-offline
```

After copying `MDVWB-arm64-offline.tar.gz` to the controller:

```bash
cd /root
rm -rf MDVWB-arm64
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
chmod +x offline-install.sh
./offline-install.sh
```

The installer preserves an existing non-empty:

```text
/etc/mdvwb/buses.json
```

It also installs the manager, systemd units and static web files, and migrates supported legacy configuration when required.

## Basic verification

```bash
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json
/usr/local/bin/mdvwb-scheduler --help
systemctl status mdvwb-manager.service --no-pager
systemctl status mdvwb-scheduler.service --no-pager
systemctl list-units 'mdvwb@*.service' --all --no-pager
```

## Documentation

| Document | Audience and purpose |
|---|---|
| [`AGENTS.md`](AGENTS.md) | Dense repository context and mandatory invariants for AI coding agents |
| [`docs/DEVELOPER.md`](docs/DEVELOPER.md) | Source architecture, protocol, MQTT contracts, tests and extension procedures |
| [`docs/INSTALLATION.md`](docs/INSTALLATION.md) | Installation, configuration, commands, update, recovery and diagnostics |
| [`docs/WEB_AND_FANCOILS.md`](docs/WEB_AND_FANCOILS.md) | Web UI, bus operations, discovery and fan-coil interaction |
| [`docs/schedules-config.md`](docs/schedules-config.md) | Schedule JSON schema, validation and MQTT contract |

## Repository

```text
https://github.com/Lex26p/MDVWB
```

## Important invariants

- one process owns one serial port;
- arbitrary bus count must remain supported;
- broadcast address `0xFF` is not used for control;
- command topics end in `/on1`;
- commands are non-retained;
- factual states and metadata are retained;
- Power is independent from Mode;
- a command contains exactly one Mode and one Speed;
- factual state is confirmed only by C0;
- discovery does not apply addresses automatically;
- web files are installed in `/var/www/mdvwb`;
- user configuration is preserved during updates.

## Working fan-coil panel

The operational panel is available at:

```text
http://<Wiren-Board-address>/fancoils/
```

It loads `/mdvwb/dashboard/config`, renders visible markers from all configured buses and subscribes to retained factual controls under `/devices/Fan-<bus>_<address>/controls/...`. The page uses one compact header and gives the remaining viewport to the floor plan. Individual and group controls open in a right-side drawer. It displays temperature, setpoint, mode, speed, power, alarm and communication state. There is no editor/admin link and no Blinds or Blok control on the user-facing page. Selecting an online fan-coil enables individual Power, Mode, Speed and SetTemp commands. Commands are published non-retained to `/on1`; the UI keeps the previous factual value until the corresponding base control confirms the requested value.


## Group fan-coil control

The working panel supports explicit multi-selection and group commands for `Power`, `Mode`, `Speed`, and `SetTemp`. Parameters are opt-in: an unchecked parameter is not changed. Every selected fan-coil receives its own non-retained `/on1` MQTT command; protocol broadcast is never used. Offline and not-yet-known devices are skipped, and successful completion is based on confirmed factual base topics.

<!-- Step 9.2 UI correction: wheel zoom, compact numbered markers, map remains interactive while side drawers are open, direct individual/group selection, no status filter in header. -->

<!-- Step 9.3 UI correction: centered free-pan map, pointer dragging, larger 1-3 digit marker numbers, stable pending control-row height, and rich marker hover tooltips. -->


### Compact dashboard editor correction

The engineering dashboard editor is now a single-header, map-first workspace. General settings and background upload share the temporary Parameters drawer, the search field is removed, markers use the approved fan icon, and every placement has a unique editable user number from 1 to 200. Existing configurations without a number are migrated sequentially.

### Dashboard editor usability update

The administrative dashboard editor is map-first. It uses the same numbered circular markers as `/fancoils/`, shows fan numbers in the device catalog, supports any configured bus IDs, and lets an administrator enable devices individually or an entire bus with checkboxes. Marker movement is snapped to a visible 1% X/Y grid. The bus administration page uses one compact top bar instead of stacked summary headers.

### Dashboard editor selection and sizing

Bus cards are displayed as full-width rows from top to bottom, so additional buses do not compress existing cards. Selecting a fan-coil in either the catalog or the map highlights the same device in both places. A short click only selects; dragging starts after pointer movement and preserves the marker's original center. Marker size is a single panel-wide setting under `Параметры` and is applied uniformly to every placement.


### Multiple independent user panels

`dashboard.json` now uses collection schema version 2. The engineering editor can create, copy, rename and delete up to 64 independent panels. Each panel has its own title, image, selected fan coils, numbers and positions. Legacy version-1 dashboard JSON is automatically migrated to panel `main`.

Public links select a panel without exposing an admin selector:

```text
/fancoils/?panel=main
/fancoils/?panel=floor-2
```

`/fancoils/` without a query opens `defaultPanel`. Background upload is located inside the selected panel's Parameters drawer and its MQTT start payload includes `panelId`, so an upload changes only that panel.


### Step 9.8 editor interaction correction

The dashboard editor keeps the 1% grid switch inside **Panel settings**. The editor header contains no zoom controls; wheel input over the map changes only the temporary editor preview scale. The saved opening scale remains the explicit setting in the drawer. Marker rotation is no longer exposed or rendered; legacy `rotation` values are accepted for compatibility and normalized to zero on the next save.


## Schedule configuration backend

The manager owns `/etc/mdvwb/schedules.json` and exposes retained configuration/status plus non-retained save and manual-run commands under `/mdvwb/schedules/...`. Weekly and one-time schedules store a panel ID, explicit individual bus/address targets, local controller time and opt-in Power/Mode/Speed/SetTemp actions. Saves use optimistic revision control and reject targets that are missing from `buses.json` or the selected visible dashboard panel. See [`docs/schedules-config.md`](docs/schedules-config.md). Actual timed execution is introduced by the separate scheduler service in the next step.
