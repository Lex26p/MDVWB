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
- systemd service synchronization through `mdvwb-manager`;
- per-bus start, stop, restart and status;
- per-bus device discovery;
- static offline web configuration interface;
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
        |      +-- /etc/default/mdvwb-N
        |      +-- mdvwb@N.service control
        |      `-- discovery for a selected bus
        |
        +-- mdvwb@1.service --> MDVWB --> RS-485 bus 1
        +-- mdvwb@2.service --> MDVWB --> RS-485 bus 2
        `-- mdvwb@N.service --> MDVWB --> RS-485 bus N
```

Each driver process owns exactly one serial port. Buses run independently and poll simultaneously.

## Bus configuration

Canonical configuration:

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
- device discovery.

Discovery results are displayed but are not automatically written to the configuration. The selected bus remains stopped after discovery.

## Build and tests

Windows with the Visual Studio CMake preset:

```powershell
cmake --preset x64-debug
cmake --build "out/build/x64-debug"
ctest --test-dir "out/build/x64-debug" -C Debug --output-on-failure
node ".\tests\mdvwb_web_model_test.mjs"
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
systemctl status mdvwb-manager.service --no-pager
systemctl list-units 'mdvwb@*.service' --all --no-pager
```

## Documentation

| Document | Audience and purpose |
|---|---|
| [`AGENTS.md`](AGENTS.md) | Dense repository context and mandatory invariants for AI coding agents |
| [`docs/DEVELOPER.md`](docs/DEVELOPER.md) | Source architecture, protocol, MQTT contracts, tests and extension procedures |
| [`docs/INSTALLATION.md`](docs/INSTALLATION.md) | Installation, configuration, commands, update, recovery and diagnostics |
| [`docs/WEB_AND_FANCOILS.md`](docs/WEB_AND_FANCOILS.md) | Web UI, bus operations, discovery and fan-coil interaction |

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
