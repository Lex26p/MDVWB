# Modbus implementation status

> Last updated: 2026-08-24
>
> This file records what has actually been completed or prepared for the Modbus work.
>
> Planning belongs in `ROADMAP.md`. Architecture belongs in `ARCHITECTURE.md` and `PROFILE_FORMAT.md`.

## Current stage

**The first profile-driven Modbus RTU software stack is implemented and passes the complete local regression suite. Real-hardware release validation and a second independent equipment profile remain open.**

Modbus RTU framing/serial transport, strict schema-v1 profile loading, semantic conversion, logical-address resolution, the first production equipment profile, protocol-aware bus/service configuration, live polling, confirmed Power/Mode/FanSpeed/integer-SetTemperature writes, MQTT integration, retained UI profile catalog, capability-aware operator/schedule control, safe discovery of logical addresses `1..63`, resolved poll plans and conservative transaction optimization are implemented.

The existing MDV runtime remains unchanged behind the same protocol-independent boundary. The per-bus systemd instance still owns exactly one process and one serial port; `mdvwb-run` now selects the MDV executable or the internal Modbus runtime from the managed protocol setting.

## Current overall status

```text
Documentation / design     CURRENT
Runtime implementation     IMPLEMENTED / LOCALLY VERIFIED
Hardware validation        PARTIAL (profile facts only)
Production release         NOT STARTED
```

## Documentation status

Prepared in the current documentation batch:

- [x] `docs/modbus/ARCHITECTURE.md`
- [x] `docs/modbus/PROFILE_FORMAT.md`
- [x] `docs/modbus/ROADMAP.md`
- [x] `docs/modbus/STATUS.md`

Also prepared in this documentation batch:

- [x] `docs/modbus/REFERENCE_VRF_ADD_CONTROLLER.md`
- [x] `docs/README.md`
- [x] `docs/protocols/MDV_V2_RESEARCH.md`

The documentation baseline was committed and verified before runtime refactoring began.

## Implementation status

- [x] Protocol-independent driver boundary
- [x] Modbus RTU transport core
- [x] Modbus CRC implementation/tests
- [x] Modbus request/response handling
- [x] Modbus exception handling
- [x] Profile loader
- [x] Profile validation
- [x] Numeric scaling and inverse write conversion
- [x] Enum mapping
- [x] Capabilities
- [x] Logical address resolver
- [x] Scan of logical addresses `1..63`
- [x] First production Modbus equipment profile
- [x] Manager/bus configuration integration
- [x] MQTT integration
- [x] Web configuration UI
- [x] Polling and transaction optimization
- [x] Capability-aware operator UI and backend schedule validation
- [x] Discovery/service ownership protection
- [x] Strict runtime profile validation and retained optional-state cleanup
- [x] Web model tests in the validation workflow
- [ ] Real hardware validation
- [x] Modbus runtime packaging/deployment handoff
- [ ] Second independent profile proving architecture reuse

## Decisions already agreed

The following decisions are considered part of the current design baseline.

### Protocol and transport

- Initial Modbus support is **Modbus RTU** over RS-485.
- Modbus TCP is not part of the first implementation.
- Only equipment with a known and reviewed register map will be supported.
- There will be no blind search for unknown Modbus equipment.

### Logical device addresses

- MDVWB uses logical air-conditioner/fan-coil addresses `1..63`.
- `1..63` is an intentional MDVWB limitation, not a Modbus protocol limit.
- A scan always evaluates the full logical range `1..63`.
- A scan does not depend on which logical devices are already configured for normal polling.

### Scan behavior

- The selected equipment profile defines a safe read-only probe.
- For every logical candidate `1..63`, the profile determines the physical Modbus request.
- No write command may be used for normal scan/probe.
- Manufacturer-specific discovery commands are not the foundation of the common scan mechanism.

### Address separation

These values are separate concepts:

```text
MDVWB logical address
Modbus Slave ID
Modbus register location
```

MDVWB must support at least:

```text
different Slave IDs + same register map
```

and:

```text
one Slave ID + different per-device register blocks
```

### Profile-driven design

- Manufacturer-specific register knowledge belongs in a profile.
- A normal new Modbus air-conditioner should usually require a new profile only.
- The common Modbus engine must not accumulate manufacturer-specific `if/else` branches.
- Profiles are data-driven JSON files using schema version 1.
- Available profiles are loaded deterministically from the configured profile directory.
- Truly unusual equipment may use a small specialized adapter, but this is an exception.

### Common semantic model

Modbus raw values should be normalized to the existing MDVWB semantic model before reaching protocol-independent code.

Expected semantic values include:

```text
Power
Mode
FanSpeed
ActiveFanSpeed where available
SetTemperature
RoomTemperature
Alarm/Error
Online/Offline
```

MQTT should continue to use semantic values rather than raw register contents.

### Numeric conversion

Profiles must support generic numeric conversion:

```text
physical = raw * scale + offset
```

and inverse write conversion:

```text
raw = (physical - offset) / scale
```

Example:

```text
raw 235
scale 0.1
-> 23.5 °C
```

Writing:

```text
22.5 °C
scale 0.1
-> raw 225
```

The mechanism is generic and is not limited to temperature.

Profiles should also support physical constraints such as:

```text
min
max
step
```

### Register conventions

The implemented profile convention is:

```text
store zero-based Modbus PDU register addresses
```

Manufacturer references such as `40028` may be preserved as documentation metadata, but must not be used ambiguously as wire addresses.

### Capabilities

Profiles declare supported capabilities.

The engineering and operator UI react to capabilities rather than profile/manufacturer names.

Example principle:

```text
capabilities.autoMode == false
-> do not show Auto Mode
```

## First reference equipment

The first reference profile is based on the supplied VRF controller Modbus data-point table.

Known architectural characteristics already identified:

- a gateway/controller may expose many indoor units through one Modbus Slave ID;
- different logical indoor units may use different repeated register blocks;
- the register blocks may be derived using a fixed stride;
- status and control registers may differ;
- the common MDVWB logical address limit remains `1..63`.

The live-confirmed subset covers addressing, discovery, Power and AlarmCode.
The production profile additionally enables a field-validation implementation
of Mode, FanSpeed, integer SetTemperature and integer RoomTemperature from the
manufacturer's published bit/register table. Those four mappings are executable
but are not yet recorded as confirmed on real equipment.

## Runtime boundary implemented

At this status point:

- `IDeviceDriver` defines protocol-independent command and state access;
- `MdvDriver` and `ModbusDriver` both implement that interface;
- MQTT command routing uses semantic `DriverCommand` values;
- MQTT state publication consumes `DriverDeviceState`, not protocol frames or raw registers;
- the Modbus runtime performs profile-driven factual polling and confirmed
  writes for Power, Mode, FanSpeed and scalar SetTemperature;
- manager-generated service configuration selects the protocol and production profile;
- `mdvwb@N.service` still launches one `mdvwb-run` wrapper per physical bus;
- the wrapper selects `/usr/local/bin/MDVWB` for MDV or the internal `/usr/local/lib/mdvwb/mdvwb-modbus` runtime for Modbus;
- `mdvwb-offline` remains shared and protocol-independent;
- the management web UI selects MDV or Modbus RTU, derives serial settings and capabilities from the selected profile, runs safe profile-driven discovery and copies confirmed found addresses into the configuration draft.

## Web configuration and discovery implemented

Milestone 10 now provides:

- a deterministic, web-safe profile catalog published as retained MQTT state;
- protocol selection between backward-compatible MDV and Modbus RTU;
- profile selection without manufacturer-specific branches in JavaScript;
- serial settings and capability summaries derived from profile metadata;
- Modbus logical-address validation against both the common `1..63` range and the selected profile range;
- discovery routing based on the manager-generated per-bus runtime environment;
- safe read-only Modbus probes for all logical candidates `1..63`;
- rejection of partial discovery results after factual transport/protocol errors;
- an explicit UI action that copies confirmed found addresses into the unsaved configuration draft.

The discovery operation stops the selected bus service and does not restart it automatically, preserving the existing operator-visible behavior. No scan performs a write, and no unknown profile is probed.

While discovery owns a bus service, manager rejects start/restart and any save
that removes or changes that bus runtime configuration. Saves affecting only
other buses remain allowed, but apply and rollback omit lifecycle actions for
the discovery-owned service.

## Polling and transaction optimization implemented

Milestone 11 now provides:

- one immutable resolved poll plan per runtime instead of resolving every semantic register on every cycle;
- explicit baseline and optimized transaction/register metrics;
- conservative FC03 batching only for exact duplicates or directly adjacent holding registers on the same Slave ID;
- reuse of one raw register value by multiple semantic points without changing semantic conversion order;
- no reads across undeclared register gaps;
- atomic factual snapshots even when a batch fails or returns the wrong size;
- established online state is retained through two consecutive failed complete
  polls; the third failure publishes offline, and the first subsequent complete
  successful poll resets the counter and restores online;
- write/confirmation failures do not independently change device availability;
- journal diagnostics identify the logical device, failing stage, Slave ID,
  register or register range, and the ordinary-poll failure counter;
- a per-bus `pollPeriodMs` start-to-start lower bound of `150..60000 ms`
  (`300 ms` by default) for logical-device operations; successful
  writes/confirmations and retries cannot accelerate scheduling below it;
- bounded configurable write attempts, confirmation attempts and priority burst before an ordinary poll;
- retry backoff that reduces repeated traffic after timeout, I/O or invalid-response outcomes.

Default behavior uses three write attempts, three confirmation attempts and at
most four priority operations before polling. The Modbus logical-device period
defaults to 300 ms and is configurable per bus up to 60000 ms;
command/confirmation work uses the configured poll period as its lower bound,
and failed work keeps the larger configured retry interval. Native MDV has its
own per-bus `150..60000 ms` period. The serial transport still enforces Modbus
RTU inter-frame timing independently.

For the current `vrf_add_controller` profile, Power, Mode, FanSpeed and integer
SetTemperature occupy adjacent registers `40028..40031` and are read in one
FC03 batch; AlarmCode at `40035` remains a separate request. No undeclared gap
is crossed.

## Runtime safety and capability enforcement

- profile integer fields are range-checked before conversion to fixed-width runtime types;
- driver startup and Modbus discovery both reject profiles that are parseable but not executable by the current runtime;
- the profile catalog preserves supported/readable semantic points but reports
  `writable=true` only for the intersection of the profile declaration and the
  current runtime implementation;
- the current production runtime performs confirmed writes for `Power`, `Mode`,
  `FanSpeed` and scalar `SetTemperature`;
- a write outside that runtime set, or a read-only point, does not expose a
  command in `/fancoils/` or permit a scheduler action;
- group plans omit unsupported controls per target;
- schedule editing uses the intersection of target capabilities;
- scheduler independently loads the current profile and rejects an unsupported action before publishing any command;
- when `Mode`, `Speed`, `SetTemp`, `Temp`, `Blinds` or `Blok` becomes unavailable, its obsolete retained base value is cleared;
- the compatibility fallback `Status=5` for an online powered device without Mode is intentional and does not publish a fabricated `Mode` fact.

## Verification status

The current tree completes a clean Windows build (160 Ninja build steps), all
42 registered CTest tests and all seven JavaScript model tests. `validate.yml` now executes
the web tests in addition to its Release build and CTest gate.

Profile-loader tests cover valid schema-v1 profiles, all three current addressing declarations, transport/register/probe validation, numeric and enum declaration validation, file loading, isolated invalid files, deterministic diagnostics and duplicate-ID rejection.

Existing MDV behavior remains protected by the full regression suite.

## Accepted milestone details

The following sections preserve the scope verified at each earlier milestone.
Statements such as “not included yet” describe that historical acceptance point;
the current state is the summary above.


## Modbus RTU transport implemented

Milestone 2 now provides:

- Modbus CRC16 calculation and validation;
- FC03 Read Holding Registers request/response handling;
- FC10 Write Multiple Registers request/response handling;
- Modbus exception-response handling;
- variable-length RTU response collection;
- configurable shared serial-port settings;
- Modbus RTU serial transport with response timeout and t3.5 inter-frame delay;
- transport tests that do not require physical hardware.

The existing MDV serial transport still opens the port explicitly as 4800 8N1, preserving current MDV behavior.

No manufacturer register map, JSON profile, Modbus bus configuration, scan logic or UI is included in this milestone.

## Profile loader and validation implemented

Milestone 3 now provides:

- strict reusable JSON parsing including decimal profile values;
- schema version `1` validation;
- stable profile identity and `pdu_zero_based` register-addressing validation;
- serial transport declaration validation;
- `direct_slave`, `fixed_slave_stride` and `explicit` addressing declarations;
- validated read/write register locations and read-only probe declarations;
- boolean, enum and numeric point declarations;
- numeric transform/limits/write-rounding declarations;
- enum `readMap` / `writeMap` declarations;
- capability-to-point consistency validation;
- deterministic non-recursive loading of `*.json` profile files from a directory;
- isolation of malformed/invalid files from unrelated valid profiles;
- rejection of every member of a duplicate profile-ID group.

No production profile is shipped yet, and no profile field is currently used to perform live semantic conversion or bus I/O.

The exact installed profile directory remains open until the packaging/configuration milestones.

## Semantic value conversion implemented

Milestone 4 now provides:

- generic scalar conversion for boolean, enum and numeric profile points;
- `uint16` and signed `int16` raw numeric representations;
- numeric scale/offset and inverse conversion;
- physical min/max/step validation before writes;
- exact/nearest/floor/ceil write rounding;
- canonical semantic Mode values (`cool`, `heat`, `dry`, `fan`, `auto`);
- canonical semantic FanSpeed values (`low`, `medium`, `high`, `auto`);
- binary semantic normalization for Power, Blinds and Blocked;
- conversion of profile reads into the protocol-independent `DriverDeviceState`;
- conversion of protocol-independent driver command values into profile-defined base register writes;
- capability gating so disabled profile capabilities are not exposed through the semantic bridge;
- rejection of unsupported semantic enum values and incompatible semantic point types during profile validation.

Semantic writes still return the profile's base write location; Milestone 5 already provides the common logical-address resolver that can apply the selected device's register offset before a future live write path is introduced.

No production equipment profile or live Modbus driver is included yet.

## Logical address resolver implemented

Milestone 5 now provides:

- strict rejection of invalid MDVWB logical addresses `0` and `64+`;
- profile-range handling where valid `1..63` candidates outside the selected profile range are reported as unsupported rather than fabricated;
- `direct_slave` resolution where logical address becomes Modbus Slave ID;
- `fixed_slave_stride` resolution where one Slave ID is combined with a deterministic non-negative register stride;
- schema-v1 validation that `firstLogicalAddress == logicalMin`, avoiding ambiguous negative register offsets;
- `explicit` per-logical-address Slave ID and register-offset resolution;
- deterministic handling of missing explicit mappings as unsupported candidates;
- effective register calculation `base address + registerOffset`;
- 16-bit effective-register overflow rejection before any bus transaction.

The resolver returns only physical addressing information. It does not send traffic, scan the bus or select configured devices.

No production equipment profile or live Modbus driver is included yet.

## Profile-driven scan implemented

Milestone 6 now provides:

- a deterministic scan plan containing every MDVWB logical candidate `1..63`;
- profile resolver application for each candidate before any bus request;
- unsupported logical candidates retained in the report without generating bus traffic;
- read-only probe execution through the existing `ITransactionTransport` boundary;
- FC03 holding-register probe requests built from the selected profile's resolved Slave ID, effective register and quantity;
- `Found` only for a valid successful response matching the planned Slave ID, function and register count;
- `NotFound` for timeout and Modbus exception probe outcomes;
- `Unsupported` for profile-unsupported candidates and probe data spaces not yet supported by the RTU core;
- `Error` for I/O failures, invalid requests, malformed/invalid responses and inconsistent successful transport results;
- preservation of Modbus exception code and diagnostic text in scan results;
- tests proving that unsupported candidates and unsupported probe data spaces generate no bus traffic;
- a full direct-addressing execution test that performs exactly 63 read-only probes;
- profile-declared presence validation with `any_response` and `any_nonzero`;
- `PresenceMismatch -> NotFound` for valid responses that do not satisfy the profile's presence rule.

The current RTU core implements FC03 and FC10 only. Therefore the scan executor currently performs live probes only when the profile probe uses `holding_register`. `input_register`, `coil` and `discrete_input` probes are reported as unsupported and generate no request until the corresponding standard read functions are added.

The scan layer does not configure serial ports, select bus profiles, persist discovered devices or perform normal polling/control. Those remain later milestones.

## First production VRF profile implemented

Milestone 7 now includes:

```text
profiles/modbus/vrf_add_controller.json
```

Confirmed live-installation facts promoted into the profile:

- 9600 8N1;
- fixed Modbus Slave ID `1`;
- manufacturer register numbers are used literally as request addresses, e.g. source `40028` is address `40028`;
- logical `1..63` maps to `Y = 0..62` with register stride `91`;
- read-only scan probe at inlet-temperature register `40039 + 91*Y`;
- absent indoor-unit observation: raw inlet temperature `0`;
- profile presence rule `any_nonzero`;
- Power enabled using status `40028 + 91*Y`, control `40078 + 91*Y`, values `0/1`;
- AlarmCode enabled read-only at `40035 + 91*Y`.

The profile also enables field-validation mappings taken directly from the
manufacturer table: Mode at `40029/40079`, FanSpeed at `40030/40080`, and the
integer part of SetTemperature at `40031/40081` with range `16..32`. Their live
behavior still requires controller verification. Half-degree registers
`40037/40085` are not used, so the current profile exposes only whole-degree
setpoints. RoomTemperature is enabled read-only as raw `uint16`, `scale=1`,
`offset=0` from `40039 + 91*Y`. Its already validated probe value is reused, so
the factual `Temp` adds no Modbus transaction. Sensor-error sentinels are still
not verified.

The current logical identity follows the controller's sorted `Y` slot. A topology change may potentially shift which physical indoor unit occupies a logical address.

Milestone 7 tests load the production JSON, verify literal addresses and stride resolution, reject invalid probe presence declarations, and verify the chosen `0 -> NotFound`, non-zero -> `Found` scan behavior through the common transaction boundary.

Normal profile-driven Modbus polling and confirmed Power, Mode, FanSpeed and
scalar SetTemperature control are implemented by `ModbusDriver`.

## Protocol-aware bus/service configuration implemented

Milestone 8 now provides:

- backward-compatible `buses.json` parsing where an omitted `protocol` still means `mdv`;
- explicit `protocol = modbus_rtu` buses with `profileId`, baud rate, data bits, parity and stop bits;
- the stricter Modbus logical-address range `1..63` while legacy MDV retains `0..63`;
- profile-catalog validation before a Modbus service plan is generated, including exact transport compatibility and profile-supported logical addresses;
- manager-generated per-bus environment fields for protocol, selected profile, profile directory and Modbus serial settings;
- automatic clearing of stale Modbus environment fields when a bus is MDV;
- an install rule for shipped JSON profiles under the MDVWB runtime support directory;
- a deployment-template update carrying the new protocol/runtime fields.

`mdvwb-run` now performs an explicit protocol split. `protocol=mdv` preserves the existing `MDVWB` invocation, while `protocol=modbus_rtu` exports the validated profile/serial/MQTT environment and executes the internal `mdvwb-modbus` runtime. There is no fall-through from Modbus configuration into the legacy MDV C0/C3 wire protocol. `--publish-offline` remains protocol-independent.

The internal runtime is installed below `/usr/local/lib/mdvwb`; it is an implementation detail of the existing per-bus service, not a fifth public MDVWB application or a second process owning the same port.

## Live Modbus MQTT runtime implemented

Milestone 9 now provides:

- a profile-driven `ModbusDriver` implementing the existing `IDeviceDriver` boundary;
- atomic factual polling snapshots using the profile presence probe followed by enabled semantic reads;
- factual publication through the existing `Power`, `Mode`, `Speed`, `SetTemp`,
  `Alarm`, `AlarmCode` and derived `Status` topics;
- existing `/devices/Fan-<bus>_<logical>/controls/<Control>/on1` command routing without manufacturer-specific MQTT topics;
- profile-driven FC10 writes to the selected control register;
- FC03 factual read-back before any commanded state is updated or published;
- bounded write/confirmation retry and ordinary-poll fairness;
- rejection of profile-disabled controls before any Modbus write traffic;
- strict managed-environment parsing and runtime profile/serial revalidation before the port is opened;
- an internal packaged `mdvwb-modbus` executable selected by `mdvwb-run`;
- ARM64/source package staging for the internal runtime and the shipped `vrf_add_controller` JSON profile;
- backward-compatible installer handoff that keeps old package-format fixtures valid, rejects torn Modbus payloads, and preserves the previous runtime/profile inside lifecycle backups;
- regression tests proving that MDV launch behavior remains unchanged and that Modbus uses the same MQTT semantic contract.

For the first production profile, normal polling reads the safe presence point,
the adjacent Power/Mode/FanSpeed/integer-SetTemperature block, and AlarmCode. A
zero presence value contributes one failed complete poll; three consecutive
failed complete polls publish the ordinary existing offline representation
(`Alarm=2`, `Status=7`). A successful FC10 response alone never changes factual
state; only matching FC03 read-back does.

The confirmed command state machine supports Power, Mode, Speed and SetTemp as
independent pending controls. Effective writability remains the intersection of
the profile declaration and runtime implementation. For
`vrf_add_controller`, those four MQTT commands are supported; Blinds and Blok
are rejected without wire traffic. Capability-aware metadata, engineering
configuration, operator controls and schedule execution use the same boundary.

## Open design items

These items are not yet final and should not be treated as implemented facts:

- exact JSON Schema file;
- hardware-tuned retry/poll timing;
- deployment and preservation policy for local custom profiles;
- 32-bit values and word order;
- specialized adapter API;
- exact persistence format for scan results;
- exact unknown-enum update behavior.

## Current risks

### Over-generalizing too early

Risk:

Trying to support every possible Modbus device before the first real device works.

Mitigation:

Implement the smallest profile schema that fully supports confirmed real equipment, then extend it when a second real profile requires new features.

### Manufacturer register notation

Risk:

Confusing documentation references such as `40001` with zero-based PDU addresses.

Mitigation:

Use explicit profile addressing convention and retain manufacturer references as metadata. For the first VRF profile, live WirenBoard verification confirms that source `40028` is used literally as request address `40028`.

### Manufacturer-specific logic leaking into core

Risk:

Adding profile-name checks throughout driver, manager, MQTT or web code.

Mitigation:

Use generic profile metadata, mappings and capabilities.

### Scan becoming manufacturer-specific

Risk:

Using a proprietary discovery feature from the first controller as the common scanning design.

Mitigation:

Common scan remains logical `1..63` using a safe profile-defined read-only probe.

## Next development step

Complete real hardware validation from `ROADMAP.md`.

The next task is field validation against actual equipment: stable
communication, scan `1..63`, factual Mode/Speed/SetTemp reads, one-at-a-time
Power/Mode/FanSpeed/integer-SetTemperature round trips, offline/recovery behavior
and multiple configured devices on one bus. Record the observed raw/factual
values before treating the three newly enabled mappings as hardware-confirmed.

Integer RoomTemperature must be compared with the physical inlet-air
temperature and checked for invalid/sensor-error values. Half-degree
SetTemperature remains disabled until its composite behavior is implemented
and verified.

## Status update rules

This file should be updated whenever an implementation step is accepted.

A normal update should record:

1. What was actually completed.
2. What files/components were changed.
3. What was verified by tests.
4. What was verified on real hardware, if applicable.
5. Any deviation from the roadmap.
6. Newly discovered limitations or open questions.
7. The next concrete development step.

Do not mark work complete merely because code was written.

A feature should be marked complete only after the agreed verification for that step succeeds.

If a status entry refers to a committed implementation milestone, record the relevant commit hash when useful.
