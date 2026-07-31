# Modbus implementation status

> Last updated: 2026-07-31
>
> This file records what has actually been completed or prepared for the Modbus work.
>
> Planning belongs in `ROADMAP.md`. Architecture belongs in `ARCHITECTURE.md` and `PROFILE_FORMAT.md`.

## Current stage

**Milestone 1 complete after successful build/CTest: protocol-independent driver boundary.**

Modbus RTU wire/profiles are still **not implemented**.

The existing MDV driver now exposes a protocol-independent command/state boundary, and MQTT consumes that boundary instead of MDV-specific command/state types.

## Current overall status

```text
Documentation / design     PREPARED
Runtime implementation     MILESTONE 1
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
- [ ] Modbus RTU transport core
- [ ] Modbus CRC implementation/tests
- [ ] Modbus request/response handling
- [ ] Modbus exception handling
- [ ] Profile loader
- [ ] Profile validation
- [ ] Numeric scaling and inverse write conversion
- [ ] Enum mapping
- [ ] Capabilities
- [ ] Logical address resolver
- [ ] Scan of logical addresses `1..63`
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
- no Modbus RTU framing/register/profile code exists yet;
- no manager configuration schema has been changed for Modbus;
- no web UI has been changed for Modbus;
- no Modbus service has been enabled;
- no installation/deployment files have been changed for Modbus.

## Verification status

Milestone 1 is considered complete only when the change is built locally and the full CTest suite passes before commit.

The refactor is designed to preserve current MQTT topics, values and MDV retry/poll behavior.

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

Begin **Milestone 2: Modbus RTU transport core** from `ROADMAP.md`.

The next task should introduce only reusable Modbus RTU framing/CRC/transaction primitives and tests. It must not yet add manufacturer register mappings, manager/web configuration or a production equipment profile.

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
