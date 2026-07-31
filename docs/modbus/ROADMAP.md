# Modbus development roadmap

> Status: planning document.
>
> Purpose: define the implementation order for Modbus RTU support in MDVWB.
>
> Related documents:
>
> - `docs/modbus/ARCHITECTURE.md`
> - `docs/modbus/PROFILE_FORMAT.md`
> - `docs/modbus/STATUS.md` (created separately and used for actual progress)

## 1. Roadmap principles

The implementation must follow these rules:

1. Existing MDV functionality must remain working throughout the project.
2. Modbus support is introduced incrementally.
3. Each development step should be independently testable.
4. Protocol-independent parts of MDVWB should remain protocol-independent.
5. Manufacturer-specific register knowledge belongs in profiles, not in the common Modbus engine.
6. Scan always evaluates logical MDVWB addresses `1..63`.
7. Only known, reviewed Modbus profiles may be scanned.
8. No blind probing of unknown equipment.
9. Normal new Modbus equipment should be added by profile rather than by modifying core C++.
10. The first implementation should support real requirements, not every theoretical Modbus feature.

## 2. Milestone 0: documentation baseline

### Goal

Freeze the agreed architecture before changing runtime code.

### Deliverables

- `docs/modbus/ARCHITECTURE.md`
- `docs/modbus/PROFILE_FORMAT.md`
- `docs/modbus/ROADMAP.md`
- `docs/modbus/STATUS.md`
- documentation index for developers/AI agents
- first reference equipment notes/profile source material
- existing MDV v2 research documentation preserved separately

### Acceptance criteria

- Core architectural decisions are documented.
- Open questions are explicitly marked as open.
- Another developer or AI can reconstruct the intended Modbus architecture from repository documentation alone.

---

## 3. Milestone 1: protocol-independent driver boundary

### Goal

Create a clean boundary between the existing MDV implementation and future Modbus implementation without changing existing application behavior.

### Main work

- Identify the minimum semantic interface required by manager/MQTT/scheduler.
- Isolate protocol-specific device communication behind a driver abstraction.
- Preserve the current common device state semantics.
- Keep MDV behavior unchanged.
- Avoid redesigning unrelated code.

Conceptually:

```text
                 Common application
                        |
                 semantic driver API
                  /             \
             MDV driver      Modbus driver
```

### Important constraint

This milestone must not implement Modbus register logic yet.

The purpose is only to make Modbus possible without spreading protocol checks through the codebase.

### Tests

- Existing MDV tests still pass.
- Existing MDV runtime behavior is unchanged.
- Driver abstraction can be exercised with the existing MDV implementation.

---

## 4. Milestone 2: Modbus RTU transport core

### Goal

Implement the reusable Modbus RTU communication layer.

### Initial scope

Support only features required by the first real profile.

Expected areas:

- RS-485 serial open/configuration.
- Modbus RTU request framing.
- CRC16.
- response framing.
- timeout handling.
- Modbus exception responses.
- transaction serialization.
- retry policy integration.
- read operations.
- write operations required by the first device.

### Likely standard operations

Exact functions will be finalized from the first profile, but likely candidates include:

```text
01 Read Coils
02 Read Discrete Inputs
03 Read Holding Registers
04 Read Input Registers
05 Write Single Coil
06 Write Single Register
15 Write Multiple Coils
16 Write Multiple Registers
```

Do not implement functions merely because Modbus defines them.

### Tests

- Known CRC vectors.
- Request frame fixtures.
- Response frame fixtures.
- exception response handling.
- timeout/retry behavior.
- invalid CRC rejection.
- wrong slave/function response rejection.

---

## 5. Milestone 3: profile loader and validation

### Goal

Load equipment profiles from data files instead of hardcoding register maps in C++.

### Main work

- Define schema version `1`.
- Load JSON profile files.
- Validate profile structure.
- Reject invalid profiles safely.
- Index valid profiles by stable `id`.
- Expose profile metadata to configuration/UI.
- Keep one invalid profile from breaking unrelated valid profiles.

### Minimum validation

- schema version;
- unique profile ID;
- register addressing notation;
- logical range;
- Slave ID validity;
- supported Modbus data spaces;
- register address validity;
- read/write declarations;
- safe read-only probe;
- scale not equal to zero;
- numeric limits;
- enum mappings;
- addressing resolver validity.

### Tests

- valid profile accepted;
- duplicate ID rejected;
- unsupported schema rejected;
- invalid register rejected;
- invalid mapping rejected;
- writable point without required write mapping rejected;
- write probe rejected;
- one invalid profile does not prevent another valid profile loading.

---

## 6. Milestone 4: semantic value conversion

### Goal

Translate raw Modbus data into the same semantic values already used by MDVWB.

### Main work

Implement generic profile-driven conversion for:

- boolean/Power;
- enumerated Mode;
- enumerated FanSpeed;
- numeric values;
- read/write scaling;
- offset;
- min/max/step;
- signed 16-bit values;
- capability declaration.

Canonical numeric conversion:

```text
physical = raw * scale + offset
```

Write conversion:

```text
raw = (physical - offset) / scale
```

### Example

```text
raw room temperature 235
scale 0.1
-> 23.5 °C
```

and:

```text
requested setpoint 22.5 °C
scale 0.1
-> write raw 225
```

### Important rule

All values leaving the Modbus layer should already be normalized to the common MDVWB semantic model.

### Tests

- scaling;
- inverse scaling;
- offset;
- signed values;
- enum read mapping;
- enum write mapping;
- invalid semantic values rejected before bus write;
- unsupported capability not exposed as supported.

---

## 7. Milestone 5: logical address resolver

### Goal

Support different physical Modbus addressing schemes behind the same logical MDVWB address range `1..63`.

### Required models

#### Direct Slave

```text
logical 1 -> slave 1 -> same registers
logical 2 -> slave 2 -> same registers
...
```

#### Fixed Slave + stride

```text
logical 1 -> slave 1 -> register block 0
logical 2 -> slave 1 -> register block + stride
...
```

#### Explicit mapping

Used only when no reliable formula exists.

### Important rule

The common application sees only:

```text
logical address 1..63
```

It must not care whether the physical device uses:

- the same Slave ID with different registers;
- different Slave IDs with the same registers;
- another profile-defined deterministic mapping.

### Tests

- logical boundary values 1 and 63;
- invalid 0 and 64 rejected;
- direct resolver;
- stride resolver;
- explicit resolver;
- register offset calculation.

---

## 8. Milestone 6: profile-driven scan 1..63

### Goal

Implement Modbus scan behavior consistent with the agreed MDVWB model.

### Required behavior

Scan always checks:

```text
1..63
```

even when normal bus configuration currently contains only a subset.

Example:

```text
configured: 1,2,3
scan:       1..63
```

For every logical candidate:

1. profile resolves physical Modbus location;
2. profile supplies a safe read-only probe;
3. engine performs the request;
4. response is validated;
5. result is reported as found/not found.

### Important rules

- No blind scan of unknown register maps.
- No write commands during discovery.
- No dependency on manufacturer-specific discovery mechanisms.
- Manufacturer-specific discovery may be added later only as an optional optimization, never as the common foundation.

### Tests

- full iteration 1..63;
- online response;
- timeout;
- Modbus exception;
- invalid response;
- skipped profile-declared unsupported candidate if such functionality is implemented.

---

## 9. Milestone 7: first real Modbus equipment profile

### Goal

Implement the first production profile using the supplied VRF controller register table.

### Known characteristics to support

- Modbus RTU.
- Gateway/controller architecture.
- One Modbus Slave ID may represent multiple indoor units.
- Different indoor units may use repeated register blocks.
- Register block addressing may use a fixed stride.
- Read and write registers may differ.
- Temperature values may require scaling.
- Power/Mode/FanSpeed values require semantic mapping.
- The common MDVWB limit remains logical addresses `1..63`.

### Work sequence

1. Re-read the manufacturer's source table.
2. Record only confirmed fields.
3. Convert manufacturer register references to zero-based PDU addresses.
4. Define safe probe point.
5. Define Power.
6. Define Mode.
7. Define FanSpeed.
8. Define SetTemperature.
9. Define RoomTemperature.
10. Define Alarm/Error where documentation is clear.
11. Define capabilities.
12. Create fixture tests from the table and/or real captures.

### Important rule

Do not infer undocumented register behavior.

If a field is ambiguous, leave it unsupported until confirmed.

---

## 10. Milestone 8: bus configuration integration

### Goal

Allow MDVWB manager/configuration to create and run Modbus buses.

### Bus settings

Expected conceptual fields:

```text
protocol
serial port
baud rate
parity
stop bits
profile ID
configured logical addresses
profile-specific installation parameters
```

### Compatibility

Existing MDV bus configuration must retain its current meaning.

Missing new fields in old configurations must not accidentally convert an MDV bus into Modbus.

### Manager responsibilities

- validate selected profile;
- pass protocol/profile information to the correct driver;
- manage service/runtime configuration;
- preserve current transactional configuration behavior.

### Tests

- existing MDV config still loads;
- valid Modbus bus loads;
- unknown profile rejected;
- invalid logical address rejected;
- address range limited to 1..63.

---

## 11. Milestone 9: MQTT integration

### Goal

Make Modbus devices behave like normal MDVWB devices to MQTT consumers.

### Principle

MQTT should use semantic values, not raw Modbus registers.

Examples:

```text
Power
Mode
Speed
SetTemp
RoomTemp
Alarm
```

The exact existing topic structure should remain unchanged unless a real incompatibility is discovered.

### Important rule

Do not create manufacturer-specific MQTT topics for ordinary profile fields.

### Tests

- Modbus state publishes through existing semantic topics.
- MQTT command produces correct profile-driven Modbus write.
- invalid command is rejected before bus transmission.
- online/offline state is handled consistently.

---

## 12. Milestone 10: web configuration UI

### Goal

Allow Modbus buses to be configured through the existing MDVWB management web UI.

### Expected UI behavior

When protocol is Modbus RTU:

- select profile;
- select/configure serial settings;
- configure profile-specific installation parameters if required;
- scan logical addresses `1..63`;
- show found/not-found result;
- select addresses to keep in normal bus configuration.

### Capabilities

The operator/user interface should use device capabilities.

For example:

```text
profile does not support Auto Mode
-> Auto Mode control is hidden/disabled
```

The UI must not contain checks such as:

```text
if profile == "some_vendor"
```

---

## 13. Milestone 11: polling and transaction optimization

### Goal

Optimize traffic only after correctness is established.

Potential optimizations:

- combine adjacent holding-register reads;
- reuse one register read for multiple semantic points;
- cache resolved physical locations;
- avoid unnecessary writes;
- tune poll intervals;
- tune retry behavior.

### Important rule

Optimization must not change profile semantics.

Do not make batching a prerequisite for the first working implementation.

Correctness comes first. RS-485 is slow, but debugging clever broken batching is slower in a more spiritual sense.

---

## 14. Milestone 12: real hardware validation

### Goal

Validate the complete stack with actual equipment.

### Required checks

- communication stability;
- scan `1..63`;
- correct discovered logical addresses;
- Power read/write;
- Mode read/write;
- FanSpeed read/write;
- SetTemperature read/write;
- RoomTemperature scaling;
- Alarm behavior;
- offline detection;
- reconnect/recovery;
- multiple devices on the same bus;
- repeated polling over an extended period.

### Safety

Use only register operations confirmed by manufacturer documentation.

Never use experimental write registers during automatic tests on real installations.

---

## 15. Milestone 13: packaging and deployment

### Goal

Ship Modbus support through the existing MDVWB installation/update process.

### Work

- package profile files;
- install them into the selected profile directory;
- preserve profiles during update according to final policy;
- include profile files in online/offline installation packages;
- validate runtime permissions;
- document installed paths.

### Tests

- fresh install;
- update from pre-Modbus release;
- offline install/update;
- profile files present after installation;
- service starts with existing MDV-only configuration.

---

## 16. Milestone 14: second independent Modbus profile

### Goal

Prove that the architecture is genuinely reusable.

The second profile should preferably use a different addressing model from the first.

For example:

```text
First profile:
fixed Slave + register stride

Second profile:
logical address -> Slave ID, same registers
```

### Success condition

Adding the second normal profile should require:

- new profile file;
- profile-specific tests/fixtures;
- documentation update;

and ideally **no change to the common Modbus engine**.

If substantial common-engine code must be modified, review whether the profile abstraction is too narrow.

---

## 17. Future optional work

Only after the first production implementation is stable:

- Modbus TCP;
- 32-bit values;
- float values;
- configurable byte/word order;
- more advanced alarm structures;
- optional manufacturer-specific discovery acceleration;
- local/custom profile directory;
- profile hot reload;
- specialized compiled adapters;
- richer diagnostics/raw-register debug tools.

These are not requirements for initial Modbus support.

---

## 18. Proposed development order

Recommended order:

```text
0. Documentation baseline
1. Driver boundary
2. Modbus RTU core
3. Profile loader/validation
4. Semantic conversions
5. Logical address resolver
6. Scan 1..63
7. First real profile
8. Bus/manager integration
9. MQTT integration
10. Web configuration
11. Optimization
12. Hardware validation
13. Packaging/deployment
14. Second independent profile
```

Some neighboring milestones may be implemented in the same code branch or release, but tests should still make their responsibilities distinguishable.

---

## 19. Definition of "first usable Modbus release"

The first usable Modbus release is reached when:

- existing MDV operation remains intact;
- Modbus RTU bus can be configured;
- one validated equipment profile is included;
- scan checks logical addresses `1..63`;
- discovered devices can be selected for normal operation;
- Power works;
- supported Mode values work;
- supported FanSpeed values work;
- SetTemperature works;
- RoomTemperature is decoded correctly;
- Alarm/offline behavior works to the extent documented by the profile;
- MQTT remains compatible with the existing semantic model;
- web configuration can manage the Modbus bus;
- deployment packages include the required profiles;
- real hardware validation has passed.

---

## 20. Definition of "profile architecture proven"

The profile architecture is considered proven only after at least two materially different Modbus equipment families work.

Minimum evidence:

1. One profile with fixed Slave ID and per-device register blocks.
2. One profile with per-device Slave IDs and a common register map.
3. Both use the same common Modbus engine.
4. Both scan through the same logical `1..63` mechanism.
5. Both expose the same semantic MDVWB state model.
6. The second profile does not require manufacturer-specific branches in common code.

---

## 21. Status tracking rule

This roadmap describes intended work.

It must **not** be edited to pretend that planned work is already complete.

Actual progress belongs in:

```text
docs/modbus/STATUS.md
```

When a development step is completed and accepted:

- update `STATUS.md`;
- record what was actually implemented;
- record important deviations from this roadmap;
- record unresolved issues;
- record the next concrete step.

If an architectural decision changes, update the architecture/profile documents first, then adjust the roadmap.

This separation prevents the planning document from becoming a historical fiction.
