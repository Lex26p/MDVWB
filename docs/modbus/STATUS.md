# Modbus implementation status

> Last updated: 2026-07-31
>
> This file records what has actually been completed or prepared for the Modbus work.
>
> Planning belongs in `ROADMAP.md`. Architecture belongs in `ARCHITECTURE.md` and `PROFILE_FORMAT.md`.

## Current stage

**Milestone 6 complete after successful build/CTest: profile-driven scan `1..63`.**

Modbus RTU framing/serial transport, schema-v1 profile loading, semantic conversion and the logical-address resolver are implemented. A production equipment profile and live register-driven Modbus device driver are still **not implemented**.

The existing MDV driver remains behind the protocol-independent boundary. Modbus now has reusable RTU transport primitives and a deterministic data-driven profile loading boundary.

## Current overall status

```text
Documentation / design     PREPARED
Runtime implementation     MILESTONE 6
Hardware validation        NOT STARTED
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
- [ ] First production Modbus equipment profile
- [ ] Manager/bus configuration integration
- [ ] MQTT integration
- [ ] Web configuration UI
- [ ] Real hardware validation
- [ ] Packaging/deployment updates
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
- Profiles are intended to be data-driven, preferably JSON.
- Available profiles should eventually be discovered automatically from profile files.
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

The proposed profile convention is:

```text
store zero-based Modbus PDU register addresses
```

Manufacturer references such as `40028` may be preserved as documentation metadata, but must not be used ambiguously as wire addresses.

### Capabilities

Profiles should declare supported capabilities.

The UI should react to capabilities rather than profile/manufacturer names.

Example principle:

```text
capabilities.autoMode == false
-> do not show Auto Mode
```

## First reference equipment

The first reference profile will be based on the supplied VRF controller Modbus data-point table.

Known architectural characteristics already identified:

- a gateway/controller may expose many indoor units through one Modbus Slave ID;
- different logical indoor units may use different repeated register blocks;
- the register blocks may be derived using a fixed stride;
- status and control registers may differ;
- the common MDVWB logical address limit remains `1..63`.

The exact register mapping has not yet been promoted to a production profile.

It must first be documented separately from the generic profile specification.

## Runtime boundary implemented

At this status point:

- `IDeviceDriver` defines protocol-independent command and state access;
- `MdvDriver` implements that interface while preserving existing MDV behavior;
- MQTT command routing uses semantic `DriverCommand` values;
- MQTT state publication consumes `DriverDeviceState` rather than `DeviceContext`/raw MDV fields;
- Modbus RTU framing/serial transport and profile schema-v1 loading/validation exist;
- no production equipment profile or live profile-driven Modbus device driver exists yet;
- no manager configuration schema has been changed for Modbus;
- no web UI has been changed for Modbus;
- no Modbus service has been enabled;
- no installation/deployment files have been changed for Modbus.

## Verification status

Milestones 1 through 6 were accepted only after local build and full CTest verification before their commits.

Profile-loader tests cover valid schema-v1 profiles, all three current addressing declarations, transport/register/probe validation, numeric and enum declaration validation, file loading, isolated invalid files, deterministic diagnostics and duplicate-ID rejection.

Existing MDV behavior remains protected by the full regression suite.


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
- a full direct-addressing execution test that performs exactly 63 read-only probes.

The current RTU core implements FC03 and FC10 only. Therefore the scan executor currently performs live probes only when the profile probe uses `holding_register`. `input_register`, `coil` and `discrete_input` probes are reported as unsupported and generate no request until the corresponding standard read functions are added.

The scan layer does not configure serial ports, select bus profiles, persist discovered devices or perform normal polling/control. Those remain later milestones.

No production equipment profile or live Modbus driver is included yet.

## Open design items

These items are not yet final and should not be treated as implemented facts:

- exact JSON Schema file;
- exact profile installation paths;
- exact Modbus library/internal implementation choice;
- exact function codes required by the first production profile;
- batching of adjacent register reads;
- retry/poll timing;
- local custom profile policy;
- 32-bit values and word order;
- specialized adapter API;
- exact web UI layout for Modbus configuration;
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

Use explicit profile addressing convention and retain manufacturer references only as metadata.

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

Begin **Milestone 7: first real Modbus equipment profile** from `ROADMAP.md`.

The next task should return to the supplied VRF controller data-point table, promote only confirmed register/addressing behavior into a production profile, keep manufacturer references separate from zero-based PDU addresses, and add fixture tests for every enabled semantic capability.

It must still avoid manager/web configuration and should not infer unresolved hardware behavior such as ambiguous register notation or composite half-degree temperature semantics.

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
