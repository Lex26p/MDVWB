# Reference equipment: VRF Add Controller

> Status: source analysis for the first Modbus reference profile.  
> Production profile: **not implemented**.  
> Source: `Data Point Table_VRF-Add Controller lock function(mini modbus&modbus).xlsx`.
>
> Purpose: preserve confirmed information from the manufacturer's data-point table, identify the mapping required by MDVWB, and explicitly record what is still unknown.
>
> Related documents:
>
> - `docs/modbus/ARCHITECTURE.md`
> - `docs/modbus/PROFILE_FORMAT.md`
> - `docs/modbus/ROADMAP.md`
> - `docs/modbus/STATUS.md`

## 1. Important rule for future implementation

This document is a reference, not executable truth.

The source spreadsheet contains several conventions that must be verified before production code/profile creation, especially register address notation and temperature encoding.

Do not silently infer missing behavior.

Milestone 7 step 1 also freezes the first-profile source facts in:

```text
tests/fixtures/modbus/vrf_add_controller_source.json
```

That fixture is deliberately **not** a loadable Modbus profile. It preserves manufacturer register references as strings, contains no executable `address` / `pduAddress` / `registerAddressing` fields, and keeps the unresolved production blockers machine-readable. A regression test fails if somebody silently converts those references into wire addresses before the convention is proven.

Use the following labels:

- **CONFIRMED FROM TABLE**: directly stated in the supplied spreadsheet.
- **MDVWB DESIGN**: project-level decision, not a manufacturer statement.
- **OPEN / VERIFY**: insufficient information for safe implementation.

## 2. Communication parameters

### CONFIRMED FROM TABLE

The controller/converter acts as Modbus slave and the upper computer acts as Modbus master.

Communication parameters:

```text
Protocol:        Modbus RTU
Physical link:   half-duplex asynchronous serial communication
Baud rate:       9600 bps
Parity:          none
Data bits:       8
Stop bits:       1
Default address: 0x01
```

The table also states a wiring distance of less than 1000 m.

Supported function codes listed by the source:

```text
0x03  Read Holding Registers / continuous read
0x10  Write Multiple Holding Registers / continuous write
0x83  Read error response
0x90  Write error response
```

### OPEN / VERIFY

The source calls the physical wiring `UART`, while the product context describes a Modbus converter. Before deployment, verify the actual electrical interface expected by the installed converter, including RS-485 transceiver/wiring details.

MDVWB architecture assumes Modbus RTU over RS-485.

## 3. Controller-level management points

### CONFIRMED FROM TABLE

The `Modbus Device` sheet defines:

| Source address | Access | Meaning |
|---:|---|---|
| `4997` | R | Number of indoor units connected/found |
| `4998` | R | Controller ready/controllable state; value `1` means normal controllable state |
| `4999` | R/W | Clear handshake/EEPROM information and start unit search |
| `4004` | R/W | Batch switch-off by refrigerant system; `64` means all connected systems |

The source describes this vendor connection process:

1. Optionally write `0x0001` to `4999` to trigger a new search.
2. Repeatedly read `4998` until it becomes `0x0001`.
3. Read `4997` to get the number of found indoor units.
4. Read indoor-unit status points one by one.

The source states a manufacturer range of:

```text
1..160 indoor units
```

### MDVWB DESIGN

MDVWB intentionally limits one logical air-conditioner bus to:

```text
1..63
```

This limit remains even though this controller may expose more devices.

### MDVWB DESIGN

The common MDVWB scan must still present and evaluate logical candidates:

```text
1..63
```

The vendor-specific `4997/4998/4999` sequence is not the generic discovery architecture for all Modbus profiles.

It may later be used by this specific profile as a readiness/auxiliary mechanism **only if required and confirmed safe**.

## 4. Indoor-unit index Y

### CONFIRMED FROM TABLE

The source defines indoor unit index:

```text
Y = 0..159
```

Indoor units are sorted in ascending order by:

1. system number;
2. indoor-unit address number.

Many per-unit registers follow:

```text
base + 91 * Y
```

Therefore the controller exposes repeated blocks with stride:

```text
91
```

### OPEN / VERIFY

The source does not explicitly guarantee that the same physical indoor unit will keep the same `Y` index after equipment is added, removed, or rediscovered.

Because `Y` is defined by sorting, a topology change may potentially shift indices.

This matters for stable MDVWB logical addressing.

### Candidate MDVWB mapping

The simplest candidate mapping is:

```text
MDVWB logical address 1 -> Y = 0
MDVWB logical address 2 -> Y = 1
...
MDVWB logical address 63 -> Y = 62
```

This is **not yet approved as production behavior**.

Before implementation, decide whether MDVWB logical identity should be:

- the sorted `Y` index; or
- a stable mapping based on the reported system number + indoor-unit address.

Do not bury this decision inside register arithmetic.

## 5. Register-address notation warning

### CRITICAL OPEN ITEM

The spreadsheet labels values such as:

```text
40026
40028
40078
4997
4998
4999
```

as `Communication protocol address`.

It is **not yet proven** whether, on the wire:

- `40028` means literal Modbus register address `40028`; or
- `40028` is conventional `4xxxx` documentation notation corresponding to another zero-based PDU address.

The existence of controller points such as `4997` makes it unsafe to assume a single convention without verification.

### Rule

Until a real request frame, vendor clarification, or known-good Modbus client configuration confirms the convention:

> Keep all addresses in this document exactly as written by the manufacturer.

Do **not** subtract `40001`, `1`, or any other offset merely because the number looks like classic Modbus reference notation.

The production profile must store an unambiguous PDU address only after this is verified.

### Milestone 7 production gate

The source fixture intentionally remains non-executable until at least one of the following resolves the convention:

1. a captured known-good FC03/FC10 RTU request showing the two-byte starting address;
2. a vendor statement that explicitly defines whether the spreadsheet values are literal protocol addresses or 4xxxx references;
3. a known-good Modbus client configuration together with the exact client address-base convention.

The minimum useful capture is a read of one unambiguous source point such as `4998`, `40026`, or `40028`. The request must include Slave ID, function, two-byte starting address, quantity and CRC.

Until such evidence exists, Milestone 7 may prepare and test source facts, but it must not claim a production profile.

## 6. Indoor-unit identification points

For indoor unit `Y`:

| Source address | Access | Meaning |
|---|---|---|
| `40026 + 91*Y` | R | System number |
| `40027 + 91*Y` | R | Indoor-unit address number |

Both are described as 1-byte values.

These two points are important candidates for:

- identifying a discovered indoor unit;
- validating that a register block represents a real unit;
- providing a stable identity separate from `Y`.

### OPEN / VERIFY

The source does not define the values returned by these registers for an unused/nonexistent `Y`.

Therefore these registers are **candidates** for the generic profile probe, but are not yet proven sufficient for safe presence detection.

## 7. Power status and control

### CONFIRMED FROM TABLE

Status:

```text
40028 + 91*Y
0 = OFF
1 = ON
```

Control:

```text
40078 + 91*Y
0 = OFF
1 = ON
```

Status and control use different source addresses.

### MDVWB semantic mapping

```text
0 -> Power OFF
1 -> Power ON
```

This field is straightforward once register-address notation is confirmed.

## 8. Mode status and control

### CONFIRMED FROM TABLE

Status point:

```text
40029 + 91*Y
```

Control point:

```text
40079 + 91*Y
```

The table describes individual bit positions:

```text
bit 0 -> Auto
bit 1 -> Cool
bit 2 -> Dry
bit 3 -> Fan
bit 4 -> Heat
```

### Likely raw masks

If the table is interpreted literally as bit positions:

```text
Auto -> 0x0001
Cool -> 0x0002
Dry  -> 0x0004
Fan  -> 0x0008
Heat -> 0x0010
```

### OPEN / VERIFY

Before writing production mappings, verify with either:

- real Modbus captures; or
- a known-good controller/client.

Specifically verify:

- whether exactly one mode bit is normally set;
- behavior if more than one bit is set;
- whether writing one mask is sufficient;
- whether unused bits must be preserved or cleared.

The profile should expose semantic Mode values, not raw bit masks.

## 9. Fan-speed status and control

### CONFIRMED FROM TABLE

Status point:

```text
40030 + 91*Y
```

Control point:

```text
40080 + 91*Y
```

The table describes:

```text
bit 0 -> Auto
bit 1 -> High (native level 5)
bit 2 -> Mid (native level 4)
bit 3 -> Low (native level 2)
bit 4 -> Super high (native level 6)
bit 5 -> Mid low (native level 3)
bit 6 -> Super low (native level 1)
```

Interpreted as bit positions, candidate masks are:

```text
Auto       -> 0x0001
High (5)   -> 0x0002
Mid (4)    -> 0x0004
Low (2)    -> 0x0008
SuperHigh  -> 0x0010
MidLow (3) -> 0x0020
SuperLow   -> 0x0040
```

### OPEN / VERIFY

MDVWB currently prefers the common semantic fan-speed model:

```text
Low
Medium
High
Auto
```

This controller exposes six fixed native levels plus Auto.

The exact normalization policy has **not been decided**.

Possible future policy may group native levels into Low/Medium/High, but no grouping should be committed until it is reviewed.

Also verify:

- whether only one bit is set at a time;
- how status behaves in Auto;
- whether Auto status can include an active native speed simultaneously.

## 10. Set temperature is a composite value

This is an important finding from the first real table.

### CONFIRMED FROM TABLE

Integer part/status:

```text
40031 + 91*Y
```

Half-degree status flag:

```text
40037 + 91*Y
bit 0:
0 -> integer
1 -> plus 0.5 °C
```

Control integer part:

```text
40081 + 91*Y
```

Control half-degree flag:

```text
40085 + 91*Y
bit 0:
0 -> integer
1 -> plus 0.5 °C
```

### Candidate semantic interpretation

The table strongly indicates a composite set temperature:

```text
SetTemperature =
    integerRegister + (halfDegreeBit ? 0.5 : 0.0)
```

Example candidate:

```text
40031 = 22
40037 bit0 = 1
-> 22.5 °C
```

Writing `22.5 °C` would likely require:

```text
40081 = 22
40085 bit0 = 1
```

### OPEN / VERIFY

This exact behavior should be confirmed on real equipment before production use.

### Architecture consequence

A simple profile model where one semantic value maps to exactly one Modbus register is not sufficient for all real equipment.

The profile system must eventually support **composite semantic values** whose read/write representation spans more than one point.

This requirement should be incorporated into the final profile schema before the first production profile is implemented.

## 11. Humidity

### CONFIRMED FROM TABLE

Status:

```text
40032 + 91*Y
```

Control:

```text
40082 + 91*Y
```

Both are described as 1-byte values.

### OPEN / VERIFY

The table does not provide:

- unit;
- range;
- scaling;
- meaning of special values.

Do not expose humidity through MDVWB until this is confirmed.

## 12. Louver state and control

### CONFIRMED FROM TABLE

Status:

```text
40033 + 91*Y
```

Control:

```text
40083 + 91*Y
```

The register contains packed fields for up to four louvers.

For each louver, the source describes:

- one Closed/Auto bit;
- a 3-bit position value with positions 1..7.

The fields are packed across the register.

### OPEN / VERIFY

Exact write behavior and interaction among the four louver fields should be tested before support is enabled.

The first Modbus release does not need to expose louver control unless required.

## 13. Actual operating status

### CONFIRMED FROM TABLE

Register:

```text
40034 + 91*Y
```

The source describes:

```text
bits 0..1:
00b -> OFF
01b -> thermo off
10b -> thermo on
11b -> alarm

bit 2:
0 -> normal
1 -> oil return

bit 3:
0 -> normal
1 -> test run

bit 4:
0 -> normal
1 -> filter reminder
```

### Potential use

This register may provide useful semantic information for:

- active operation state;
- alarm state;
- maintenance/filter indication.

Exact mapping to the existing MDVWB `DeviceState` should be decided during implementation.

## 14. Alarm code

### CONFIRMED FROM TABLE

Register:

```text
40035 + 91*Y
```

Meaning:

```text
0      -> no alarm indicated by this code
nonzero -> alarm code
```

This is a strong candidate for the common MDVWB alarm/error field.

A vendor alarm-code dictionary was not included in the supplied table.

## 15. Air temperatures

### CONFIRMED FROM TABLE

Outlet air temperature:

```text
40038 + 91*Y
```

Inlet air temperature:

```text
40039 + 91*Y
```

Both are described as 1-byte values.

### Candidate MDVWB mapping

`Inlet air temperature` is the most natural candidate for:

```text
RoomTemperature
```

### OPEN / VERIFY

The source does not specify:

- scale;
- offset;
- signed/unsigned interpretation;
- invalid/sensor-error sentinel values.

Do not assume `/10`, direct degrees, or an offset until verified.

This is exactly the type of field for which the profile transformation system is required.

## 16. EEV values

### CONFIRMED FROM TABLE

The source lists:

```text
40040 + 91*Y -> EEV 1 opening value
40041 + 91*Y -> EEV 2 opening value
```

The table text around byte width/lower-byte meaning is not sufficiently clear for a production mapping.

These values are not required for the first common fan-coil UI.

### OPEN / VERIFY

Leave unsupported unless a concrete requirement appears.

## 17. Controller lock functions

### CONFIRMED FROM TABLE

The source provides read/write lock registers:

| Source address | Meaning |
|---|---|
| `40072 + 91*Y` | All-function lock |
| `40073 + 91*Y` | Power ON/OFF lock |
| `40074 + 91*Y` | Mode lock |
| `40075 + 91*Y` | Fan-speed lock |
| `40076 + 91*Y` | Louver lock |
| `40077 + 91*Y` | Set-temperature lock |

Values:

```text
0 -> Lock
1 -> Unlock
```

### Architecture note

This equipment provides more granular lock capabilities than a single generic lock flag.

The first implementation may expose only the common capability needed by MDVWB, but the profile design should not destroy the source distinction if granular locks become useful later.

## 18. Filter reset

### CONFIRMED FROM TABLE

Control register:

```text
40086 + 91*Y
bit 0:
0 -> normal
1 -> filter reset
```

This is a command-like field rather than persistent semantic state.

It is not required for the first basic Modbus integration.

## 19. First-profile minimum semantic set

For the first usable implementation, prioritize only fields needed by the current MDVWB user model.

Candidate minimum:

```text
Identity
Power
Mode
FanSpeed
SetTemperature
RoomTemperature
AlarmCode
Online/Offline
```

Possible source points:

| MDVWB semantic field | Source status | Source control | Current confidence |
|---|---|---|---|
| Identity | `40026/40027 + 91*Y` | — | High |
| Power | `40028 + 91*Y` | `40078 + 91*Y` | High |
| Mode | `40029 + 91*Y` | `40079 + 91*Y` | Medium; bitmask behavior to verify |
| FanSpeed | `40030 + 91*Y` | `40080 + 91*Y` | Medium; normalization policy unresolved |
| SetTemperature | `40031` + `40037` | `40081` + `40085` | Medium/high; composite behavior to verify |
| RoomTemperature | `40039 + 91*Y` | — | Medium; numeric encoding unknown |
| AlarmCode | `40035 + 91*Y` | — | High |

All addresses above are quoted in the manufacturer's notation and include `+ 91*Y` where applicable.

## 20. Scan/probe for this profile

### MDVWB DESIGN

The UI/common scan must evaluate logical addresses:

```text
1..63
```

### OPEN / VERIFY

A production-safe presence rule for this controller is not yet proven.

Potential data sources include:

- system number at `40026 + 91*Y`;
- indoor-unit address at `40027 + 91*Y`;
- controller count at `4997`;
- controller readiness at `4998`.

The source does not state what unused `Y` blocks return.

Therefore do not yet define:

```text
"probe": ...
```

in a production profile.

### Recommended hardware test

For a controller with fewer than 63 connected units:

1. Read `4997`.
2. Read identity points for known populated `Y` values.
3. Read identity points for several `Y` values beyond the reported count.
4. Record whether the controller:
   - times out;
   - returns Modbus exception;
   - returns zero;
   - returns `0xFFFF`;
   - returns stale/other data.
5. Repeat after a vendor search/restart.
6. Confirm whether `Y` ordering remains stable.

This test will determine the safest generic probe rule for this profile.

## 21. Required verification before production profile

Before the first JSON profile is considered production-ready, confirm:

1. Exact register-address convention on the wire.
2. Whether default Modbus Slave ID is actually `1` in the target installation.
3. Whether `Y = logicalAddress - 1` is an acceptable stable mapping.
4. Presence/absence behavior for unused `Y`.
5. Mode register bitmask behavior.
6. Fan-speed register bitmask behavior.
7. Desired six-native-speed to MDVWB Low/Medium/High mapping.
8. Set-temperature composite read behavior.
9. Set-temperature composite write behavior.
10. Inlet-temperature encoding/scaling.
11. Alarm-code behavior on real faults if possible.
12. Whether controller readiness at `4998` must be checked before normal polling.

## 22. Impact on generic profile format

This first real table reveals at least one requirement that must be represented by the final profile schema:

### Composite semantic values

A semantic value may require multiple Modbus points.

Set temperature is the immediate example:

```text
integer register + half-degree flag
```

Therefore the first production schema cannot assume:

```text
one semantic field == one Modbus register
```

The generic design should support a small, deterministic composite mechanism without turning profiles into a scripting language.

This requirement must be reflected in `PROFILE_FORMAT.md` before implementation begins.

## 23. What must not be hardcoded into the common engine

The following values belong to this profile only:

```text
9600 baud default
Slave ID default 1
stride 91
40026...
40086...
mode bit positions
fan-speed bit positions
controller points 4997/4998/4999
```

The common engine should only understand generic concepts such as:

```text
holding register
read/write
logical address resolver
bit extraction
enum mapping
numeric/composite conversion
capabilities
probe
```

## 24. Current conclusion

The supplied table is sufficient to justify this equipment as the first Modbus reference profile, but it is **not yet sufficient for a safe production profile without a few targeted hardware checks**.

The strongest confirmed parts are:

- Modbus RTU communication parameters;
- function codes `0x03` and `0x10`;
- repeated indoor-unit block with stride `91`;
- separate status and control registers;
- Power fields;
- identity fields;
- alarm code;
- lock functions;
- six fixed fan-speed states plus Auto;
- set-temperature integer and half-degree components.

The most important unresolved items are:

- exact wire register-address convention;
- stable mapping between MDVWB logical address and manufacturer `Y`;
- unused-`Y` behavior for scanning;
- native fan-speed normalization;
- inlet-temperature encoding;
- exact composite set-temperature write/read behavior.

Do not implement these unresolved items by assumption.
