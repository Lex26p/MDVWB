# Modbus RTU support architecture

> Status: design approved in principle, implementation not started.
>
> Purpose: this document defines the architecture for adding Modbus RTU air-conditioner/fan-coil support to MDVWB while minimizing changes to the existing application when new equipment profiles are added.
>
> Audience: MDVWB developers and AI agents working on future implementation.

## 1. Goals

MDVWB must support air-conditioners/fan-coils using Modbus RTU in addition to the existing MDV protocol.

The implementation should preserve the existing external device model wherever possible:

- Power
- Mode
- Fan speed
- Set temperature
- Room temperature
- Alarm/error state
- Device online/offline state
- Other capabilities when a profile explicitly supports them

The MQTT layer, scheduler, user dashboard and other protocol-independent parts of MDVWB should not need to know whether a device is controlled through MDV or Modbus.

The main architectural goal is:

> Adding a normal new Modbus device should usually require adding a profile, not rewriting the Modbus engine or the main application.

## 2. Scope

Initial scope:

- Modbus RTU only.
- RS-485 transport.
- Logical MDVWB device addresses are limited to `1..63`.
- Only equipment with a known and reviewed register map is supported.
- Device discovery is performed only using a known profile.
- No blind probing of unknown Modbus equipment.
- No attempt to automatically identify an unknown manufacturer or register map.
- Existing MDV functionality must remain independent and unchanged by normal Modbus profile additions.

Not in the initial scope:

- Modbus TCP.
- Generic SCADA functionality.
- Arbitrary user-created register maps through the web UI.
- Blind scan of all Modbus Slave IDs and arbitrary registers.
- Supporting more than 63 logical air-conditioners on one MDVWB bus.
- Automatic protocol detection.

## 3. Core abstraction

The application should separate three concepts that must not be treated as the same address:

1. **MDVWB logical address**
   - Always `1..63`.
   - This is the address used by the rest of MDVWB.
   - This address identifies a logical fan-coil/air-conditioner inside the application.

2. **Modbus Slave ID**
   - The actual Modbus RTU slave/unit address.
   - It may be the same for many logical devices when a manufacturer uses a gateway/controller.
   - It may also be different for every logical device.

3. **Register location**
   - The register or coil addresses used for one logical device.
   - Registers may be identical for every Slave ID.
   - Registers may instead change according to the logical device index while the Slave ID remains constant.

These three values must remain conceptually separate.

## 4. Supported addressing models

The profile system must support at least the following two common patterns.

### 4.1 Different Slave IDs, same register map

Example:

```text
MDVWB address 1 -> Modbus Slave 1 -> registers 100, 101, 102...
MDVWB address 2 -> Modbus Slave 2 -> registers 100, 101, 102...
MDVWB address 3 -> Modbus Slave 3 -> registers 100, 101, 102...
```

In this model the logical MDVWB address may map directly to the Modbus Slave ID.

### 4.2 One Slave ID, different register ranges

Example:

```text
MDVWB address 1 -> Modbus Slave 1 -> register block A
MDVWB address 2 -> Modbus Slave 1 -> register block B
MDVWB address 3 -> Modbus Slave 1 -> register block C
```

This is typical of a gateway/controller exposing multiple indoor units through one Modbus Slave ID.

A profile may calculate registers using a base address and a stride, for example conceptually:

```text
register(device) = base + stride * (logicalAddress - 1)
```

The exact formula is profile-specific.

### 4.3 Future addressing models

The architecture must allow a profile to define another deterministic mapping if a future device requires it.

The main Modbus engine must not contain manufacturer-specific address formulas.

## 5. Logical address range

MDVWB intentionally limits Modbus air-conditioners/fan-coils to:

```text
1..63
```

This is an application-level limit, not a Modbus protocol limit.

Reasons:

- Keeps Modbus devices aligned with the existing MDVWB device model.
- Keeps MQTT/device addressing consistent.
- Simplifies discovery, configuration and UI.
- Avoids spreading Modbus-specific maximum-address rules into the rest of the application.

A profile may technically describe equipment capable of more devices, but MDVWB will only expose logical addresses `1..63`.

## 6. Discovery / scan model

The scan operation always evaluates the complete logical address range:

```text
1..63
```

It does **not** scan only devices already configured on the bus.

Example:

```text
Configured before scan: 1, 2, 3

Scan still checks:
1, 2, 3, 4, 5, ... 63
```

The result may be:

```text
1  online
2  online
3  online
4  no response
5  online
...
63 no response
```

The user may then choose which discovered logical addresses should become permanently configured devices on that bus.

## 7. Discovery is profile-driven, not blind

MDVWB must never attempt to discover unknown Modbus devices without knowing how to safely query them.

For each logical address `1..63`, the selected profile must be able to produce a safe read-only probe operation.

Conceptually:

```text
Probe(logicalAddress)
    -> slaveId
    -> register/coils address
    -> register type
    -> read function
    -> expected response validation
```

A logical device is considered found only when the profile receives a valid response that satisfies its probe rules.

The probe must be read-only.

Manufacturer-specific discovery commands may exist, but they are not the foundation of MDVWB scanning. The common scan mechanism remains `1..63` using the selected profile.

## 8. Profile-driven architecture

The preferred architecture is:

```text
                     Common MDVWB device model
                               |
                        DeviceState / commands
                         /                 \
                  MDV implementation     Modbus engine
                                             |
                                      Modbus profile
                                             |
                                        Modbus RTU
```

The common Modbus engine is responsible for protocol mechanics.

A device profile is responsible for manufacturer/model-specific meaning.

### Modbus engine responsibilities

The common engine should own:

- Modbus RTU framing.
- CRC handling.
- Serial communication.
- Timeouts.
- Retries.
- Standard Modbus exception handling.
- Read/write transactions.
- Poll scheduling.
- Scan iteration over logical addresses `1..63`.
- Profile loading and validation.
- Converting profile-defined raw values to/from the common MDVWB model.
- Online/offline state.
- Transaction serialization on the RS-485 bus.

### Profile responsibilities

A profile should describe:

- Human-readable profile name.
- Stable profile identifier.
- Supported serial settings/defaults.
- Addressing model.
- Mapping from logical address to Slave ID.
- Mapping from logical address to register addresses.
- Safe discovery/probe field.
- Register/coil type.
- Read operation.
- Write operation.
- Value encoding.
- Value decoding.
- Power mapping.
- Mode mapping.
- Fan-speed mapping.
- Temperature mapping.
- Alarm/error mapping.
- Supported capabilities.
- Valid ranges and steps.
- Any deterministic per-device register offset/stride rules.

Manufacturer-specific register knowledge should remain in the profile whenever possible.

## 9. Profiles should be data-driven

Normal profiles should be stored as data files, preferably JSON.

Conceptual locations:

```text
/usr/share/mdvwb/modbus-profiles/
```

for profiles shipped with MDVWB, and potentially:

```text
/etc/mdvwb/modbus-profiles/
```

for local/custom profiles if this is later required.

The application should discover available profile files automatically rather than requiring a central C++ list to be edited every time a profile is added.

The exact installation paths and precedence rules are implementation details to be defined later.

## 10. Value transformations

A Modbus register value is not assumed to be identical to the physical value used by MDVWB.

Every numeric data point must be able to define a transformation.

Minimum required model:

```text
physical = raw * scale + offset
```

For writable values the inverse transformation is:

```text
raw = (physical - offset) / scale
```

Example:

```text
raw room temperature = 235
scale = 0.1
offset = 0

physical temperature = 23.5 °C
```

For a writable set temperature:

```text
requested physical value = 22.5 °C
scale = 0.1

raw value written to Modbus = 225
```

The transformation mechanism is generic and must not be hardcoded only for temperature.

It may also be needed for pressure, humidity, limits, percentages or future data points.

## 11. Numeric constraints

A writable numeric point should be able to define:

- Minimum physical value.
- Maximum physical value.
- Step.
- Scale.
- Offset.
- Raw data type when necessary.
- Signed/unsigned representation when necessary.

Conceptual example:

```json
{
  "setTemp": {
    "scale": 0.1,
    "offset": 0,
    "min": 16.0,
    "max": 30.0,
    "step": 0.5
  }
}
```

Validation should occur before a write is sent.

## 12. Enumerated mappings

Power, Mode and FanSpeed frequently use manufacturer-specific numeric values.

Profiles must support explicit mapping tables instead of assuming common numeric codes.

Conceptual example:

```json
{
  "mode": {
    "values": {
      "0": "cool",
      "1": "heat",
      "2": "dry",
      "3": "fan"
    }
  }
}
```

Another manufacturer may use completely different raw values without requiring changes to the common Modbus engine.

The same principle applies to fan speed.

## 13. Read and write locations may differ

A profile must not assume that a state is read and written through the same register.

For example:

```text
Power status register  = 40028
Power control register = 40078
```

Therefore each logical point may define independent read and write locations.

Read-only and write-only points must also be representable.

## 14. Register types and operations

The profile format must be capable of distinguishing standard Modbus data spaces where required:

- Coil.
- Discrete Input.
- Holding Register.
- Input Register.

The engine should support only the Modbus functions actually needed by supported profiles, but the architecture must not assume that every device uses the same function code.

Initial likely operations include standard read and single/multiple write operations, subject to the first supported device profile.

The exact function set will be finalized during implementation.

## 15. Common semantic device model

Modbus-specific raw values should be normalized before leaving the Modbus layer.

The rest of MDVWB should consume semantic state such as:

```text
power
mode
fanSpeed
activeFanSpeed
setTemperature
roomTemperature
alarm/errors
online/offline
```

Where a profile does not support a capability, that fact should be explicit.

MQTT should continue to represent semantic device state, not raw Modbus register values.

## 16. Capabilities

Profiles should explicitly describe available functions.

Examples:

```text
power
cool
heat
dry
fan
autoMode
low/medium/high fan
autoFan
roomTemperature
setTemperature
blinds
lock
alarmCode
```

The UI should use capabilities rather than manufacturer/profile-name checks.

Bad:

```text
if profile == "Manufacturer_X":
    hide Auto
```

Preferred:

```text
if !capabilities.autoMode:
    hide Auto
```

This keeps the web UI independent from individual equipment models.

## 17. Fan speed abstraction

The normal MDVWB UI should preserve the existing semantic fan-speed model wherever possible:

```text
Low
Medium
High
Auto
```

If a Modbus device has more physical steps, the profile is responsible for mapping those values to the MDVWB semantic model.

The profile may also define which raw value should be written when the user chooses Low, Medium or High.

Do not expose manufacturer-specific speed counts globally unless future requirements prove that the common model itself must change.

## 18. Bus configuration

A Modbus bus will conceptually need:

```text
protocol = Modbus RTU
serial port
baud rate
parity
stop bits
selected profile
configured logical device addresses
profile-specific bus parameters if required
```

The exact JSON schema is not defined in this architecture document.

That schema belongs in the profile/configuration specification.

## 19. Profile-specific device location

The configuration must allow the selected profile to resolve each logical address to its physical Modbus location.

Depending on the profile, this may require:

```text
logical address only
```

or:

```text
logical address + Slave ID
```

or:

```text
logical address + gateway/device index
```

or another small profile-specific parameter set.

Avoid storing a complete duplicate register map for every device when the profile can calculate it from a base/stride rule.

At the same time, the architecture must allow explicit per-device register locations if a manufacturer provides no useful formula.

## 20. Extensibility rule

The expected process for adding a conventional new Modbus device is:

```text
manufacturer register table
        |
        v
analyze known fields
        |
        v
create new Modbus profile
        |
        v
validate profile
        |
        v
profile becomes selectable
```

The common Modbus engine should not need changes.

If a future device requires behavior that cannot reasonably be expressed by the profile format, the architecture may provide a small specialized adapter/handler extension point.

Such adapters are exceptions.

Do not put manufacturer-specific branches throughout the common driver.

## 21. Profile validation

A malformed profile must fail safely before bus communication begins.

Validation should eventually check at least:

- Unique profile ID.
- Supported schema version.
- Valid Modbus data type.
- Valid function/read/write declaration.
- Valid register addresses.
- Valid scale.
- Valid min/max/step.
- Valid enum mappings.
- Valid logical-address mapping.
- Presence of a safe discovery/probe definition.
- No write operation used as a discovery probe.
- Required semantic fields for the intended device type.

Profile validation should produce a clear diagnostic identifying the profile and invalid field.

## 22. Failure behavior

A failure to read one logical device must not invalidate the whole bus.

Expected model:

```text
device 1 -> online
device 2 -> timeout/offline
device 3 -> online
```

Modbus exception responses must be distinguished from transport timeouts when practical.

A failed or unknown value for one optional field should not automatically destroy all successfully decoded state if the profile and transaction design allow safe partial handling.

Exact retry/freshness policy will be defined during implementation.

## 23. Configuration compatibility

Adding Modbus support must not silently change the meaning of existing MDV bus configurations.

Existing configurations without new protocol/profile fields should retain their current MDV behavior.

Any schema extension should prefer backward-compatible defaults where doing so is unambiguous.

## 24. First reference profile

The first reference Modbus profile will be based on the supplied VRF controller register table.

Important characteristics already identified for that class of equipment:

- A gateway/controller may use one Modbus Slave ID for multiple indoor units.
- Each indoor unit may use a different register block.
- The register block may be calculated from an index/stride.
- Read and control registers may be different.
- Manufacturer-specific discovery exists, but MDVWB will not depend on it as the common discovery architecture.

The final exact profile definition must be derived from the source register table during the profile-format implementation step.

## 25. Non-goals and anti-patterns

Do not implement the following as the base architecture:

### Manufacturer branches in common code

Avoid:

```text
if manufacturer == A ...
else if manufacturer == B ...
else if manufacturer == C ...
```

### Raw Modbus values in MQTT

Do not expose raw register values when MDVWB already has a semantic representation.

### Blind discovery

Do not probe arbitrary Slave IDs/registers hoping to identify equipment.

### Full register maps duplicated per device

Do not duplicate hundreds of register definitions for logical addresses `1..63` when a profile can express the mapping formula.

### Assuming logical address equals Slave ID

This is valid for some devices but false for gateway-based equipment.

### Assuming read register equals write register

This is manufacturer-specific and must be described by the profile.

### Hardcoded `/10` temperature logic

Scaling belongs in profile transformations.

## 26. Architectural acceptance criteria

The design should be considered successfully implemented when all of the following are true:

1. Existing MDV buses continue to work without profile-specific Modbus knowledge.
2. A Modbus RTU bus can select a known equipment profile.
3. Scan checks logical addresses `1..63`, independent of the currently configured address list.
4. The profile determines how each logical address is translated to Modbus Slave ID/registers.
5. One-Slave/many-register-block and many-Slave/same-register-map devices are both supported.
6. Numeric scaling such as `235 -> 23.5 °C` works declaratively.
7. Writable scaling performs the inverse conversion correctly.
8. Enum values such as Mode and FanSpeed are profile-defined.
9. Read and write registers may differ.
10. Supported device capabilities are profile-defined.
11. A normal new device profile can be added without editing the main Modbus engine.
12. MQTT and the rest of MDVWB operate on normalized semantic state rather than raw Modbus values.

## 27. Open design items

The following details are intentionally left for later documents/implementation:

- Exact JSON profile schema.
- Profile schema versioning.
- Exact profile installation directories.
- Exact Modbus library or internal implementation choice.
- Transaction grouping/optimization for adjacent registers.
- Poll interval and retry defaults.
- Profile-specific bus parameters.
- UI representation of profile selection.
- Persistence format for scan results and selected addresses.
- Exact Modbus function codes required by the first profile.
- Handling of 32-bit values, register byte order and word order if required by future profiles.
- Specialized adapter API for profiles that cannot be expressed declaratively.

These items should be resolved without violating the core decisions in this document.
