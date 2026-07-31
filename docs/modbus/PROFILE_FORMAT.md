# Modbus profile format

> Status: design specification, implementation not started.
>
> Purpose: define a data-driven profile format for Modbus RTU air-conditioners/fan-coils in MDVWB.
>
> Related document: `docs/modbus/ARCHITECTURE.md`.

## 1. Purpose of a profile

A Modbus profile describes how one known equipment family maps its Modbus data to the common MDVWB device model.

A normal new Modbus air-conditioner should be added by creating and validating a new profile file rather than changing the common Modbus engine.

A profile defines:

- serial communication defaults;
- how logical MDVWB addresses `1..63` map to Modbus Slave IDs and register offsets;
- which Modbus points are read and written;
- how raw values are converted to physical/semantic values;
- Power/Mode/FanSpeed mappings;
- supported capabilities;
- a safe read-only probe used when scanning logical addresses `1..63`.

Profiles are intended to be JSON files.

## 2. Separation of profile and bus configuration

The profile describes the equipment family.

The bus configuration describes one installed bus.

Example:

```text
Profile:
    how this manufacturer/model works

Bus configuration:
    serial port
    selected profile ID
    enabled state
    configured logical addresses
    installation-specific profile parameters, if any
```

The profile must not contain the Linux serial device path.

The same profile may be used by multiple buses.

## 3. Logical MDVWB address

For Modbus air-conditioners, MDVWB uses an artificial logical address range:

```text
1..63
```

This is not the Modbus protocol limit.

The selected profile must be able to resolve every candidate logical address from `1` through `63` during a scan, even if only a few devices are currently configured for normal polling.

Example:

```text
Configured devices: 1, 2, 3

Scan candidates:
1, 2, 3, 4, 5, ... 63
```

A profile therefore needs a deterministic address resolver for the whole `1..63` range.

## 4. Register address notation

This is a critical rule.

Modbus manufacturer documents commonly show references such as:

```text
40001
40028
40119
```

These human-readable references are often one-based and include a data-space prefix. The actual Modbus PDU address is normally zero-based.

To avoid permanent off-by-one bugs, profile files should store the actual protocol address used in the request.

Recommended rule:

```text
profile address = zero-based Modbus PDU address
```

A profile may additionally keep the manufacturer's reference as documentation:

```json
{
  "address": 27,
  "reference": "40028"
}
```

The engine must use `address`, not `reference`.

The profile schema should explicitly declare:

```json
{
  "registerAddressing": "pdu_zero_based"
}
```

Profiles using ambiguous register notation should fail validation.

## 5. Proposed top-level structure

Conceptual profile:

```json
{
  "schemaVersion": 1,
  "id": "vendor_model_name",
  "name": "Vendor Model Name",
  "transport": {},
  "addressing": {},
  "capabilities": {},
  "probe": {},
  "points": {}
}
```

Top-level fields:

| Field | Meaning |
|---|---|
| `schemaVersion` | Profile schema version |
| `id` | Stable machine-readable profile identifier |
| `name` | Human-readable profile name |
| `registerAddressing` | Register address notation used in the file |
| `transport` | Serial defaults/constraints |
| `addressing` | Logical address to Modbus location mapping |
| `capabilities` | Functions available on this equipment |
| `probe` | Safe read-only operation for scan |
| `points` | Semantic MDVWB data points |

## 6. Profile identity

Example:

```json
{
  "schemaVersion": 1,
  "id": "midea_vrf_add_controller",
  "name": "Midea VRF Add Controller",
  "registerAddressing": "pdu_zero_based"
}
```

Requirements:

- `id` must be unique.
- `id` must remain stable once released.
- `name` may be changed for readability.
- filename should normally match the profile ID:

```text
midea_vrf_add_controller.json
```

## 7. Transport defaults

Example:

```json
{
  "transport": {
    "baudRate": 9600,
    "dataBits": 8,
    "parity": "none",
    "stopBits": 1
  }
}
```

Initial supported values should be deliberately limited to combinations required by actual supported equipment.

The profile provides defaults. The final decision on which fields a bus may override belongs to the implementation/configuration specification.

## 8. Common physical address resolver

The common engine should resolve a logical MDVWB address into:

```text
slaveId
registerOffset
```

Then a normal point is located as:

```text
effective register = point address + registerOffset
```

This simple model covers the two main equipment types already identified.

## 9. Addressing type: direct Slave ID

Use when each air-conditioner has its own Modbus Slave ID and all devices share the same register map.

Conceptual profile:

```json
{
  "addressing": {
    "type": "direct_slave",
    "logicalMin": 1,
    "logicalMax": 63,
    "slaveId": {
      "source": "logicalAddress"
    },
    "registerOffset": 0
  }
}
```

Resolution:

```text
logical 1 -> slave 1 -> offset 0
logical 2 -> slave 2 -> offset 0
logical 3 -> slave 3 -> offset 0
...
logical 63 -> slave 63 -> offset 0
```

## 10. Addressing type: fixed Slave + register stride

Use when one Modbus gateway represents many indoor units and each unit has its own repeated register block.

Conceptual profile:

```json
{
  "addressing": {
    "type": "fixed_slave_stride",
    "logicalMin": 1,
    "logicalMax": 63,
    "slaveId": 1,
    "firstLogicalAddress": 1,
    "registerStride": 91
  }
}
```

Resolution:

```text
logical 1 -> slave 1 -> offset 0
logical 2 -> slave 1 -> offset 91
logical 3 -> slave 1 -> offset 182
...
```

Formula:

```text
registerOffset =
    (logicalAddress - firstLogicalAddress) * registerStride
```

The main engine performs the generic formula. The number `91` belongs only to the profile.

## 11. Addressing type: explicit map

Some equipment may not have a useful arithmetic relationship between logical address, Slave ID and register block.

The profile format should therefore support an explicit mapping.

Conceptual example:

```json
{
  "addressing": {
    "type": "explicit",
    "logicalMin": 1,
    "logicalMax": 63,
    "devices": {
      "1": {
        "slaveId": 1,
        "registerOffset": 0
      },
      "2": {
        "slaveId": 1,
        "registerOffset": 137
      },
      "3": {
        "slaveId": 7,
        "registerOffset": 0
      }
    }
  }
}
```

For scan purposes an explicit profile should provide mappings for every logical candidate that is valid for that equipment family.

If a logical candidate is intentionally impossible, the profile may mark it unsupported rather than causing a blind Modbus request.

## 12. Per-device point overrides

A single `registerOffset` is not sufficient if a manufacturer assigns unrelated register addresses to different devices.

The profile format should provide an escape hatch for explicit point overrides.

Conceptually:

```json
{
  "addressing": {
    "type": "explicit",
    "devices": {
      "1": {
        "slaveId": 1,
        "pointOverrides": {
          "power": {
            "read": 100,
            "write": 150
          }
        }
      }
    }
  }
}
```

This should be a fallback, not the preferred representation.

When a simple base/stride relationship exists, use the formula instead of copying full register maps 63 times.

## 13. Data spaces

A point location must state its Modbus data space.

Initial vocabulary:

```text
coil
discrete_input
holding_register
input_register
```

Example:

```json
{
  "space": "holding_register",
  "address": 27
}
```

The engine maps the data space and operation to the appropriate standard Modbus function.

Profiles should not require users to memorize function numbers when the operation can be derived safely from the declared data space and read/write action.

If a specific function choice is genuinely required, the schema may allow an explicit validated override.

## 14. Point model

A semantic point may contain independent read and write locations.

Example:

```json
{
  "power": {
    "type": "enum",
    "read": {
      "space": "holding_register",
      "address": 27,
      "reference": "40028"
    },
    "write": {
      "space": "holding_register",
      "address": 77,
      "reference": "40078"
    }
  }
}
```

This explicitly supports equipment where status and control registers differ.

## 15. Read-only point

Example:

```json
{
  "roomTemperature": {
    "type": "number",
    "read": {
      "space": "input_register",
      "address": 199
    }
  }
}
```

No `write` section means MDVWB must never attempt to write this point.

## 16. Write-only point

Write-only points should be supported only when a real device requires them.

Example:

```json
{
  "resetAlarm": {
    "type": "command",
    "write": {
      "space": "holding_register",
      "address": 300
    }
  }
}
```

A write-only command must not be used as a scan probe.

## 17. Numeric transformations

Raw Modbus values must be converted to physical MDVWB values declaratively.

Canonical read transformation:

```text
physical = raw * scale + offset
```

Canonical write transformation:

```text
raw = (physical - offset) / scale
```

Example:

```json
{
  "roomTemperature": {
    "type": "number",
    "read": {
      "space": "holding_register",
      "address": 40
    },
    "transform": {
      "scale": 0.1,
      "offset": 0
    }
  }
}
```

Result:

```text
raw 235 -> 23.5 °C
```

## 18. Writable temperature example

Example:

```json
{
  "setTemperature": {
    "type": "number",
    "read": {
      "space": "holding_register",
      "address": 41
    },
    "write": {
      "space": "holding_register",
      "address": 91
    },
    "transform": {
      "scale": 0.1,
      "offset": 0
    },
    "limits": {
      "min": 16.0,
      "max": 30.0,
      "step": 0.5
    }
  }
}
```

Read:

```text
raw 225 -> 22.5 °C
```

Write:

```text
MDVWB 22.5 °C -> raw 225
```

The engine must perform the inverse transformation automatically.

## 18.1 Composite semantic values

A real equipment profile may require one semantic MDVWB value to be assembled from more than one Modbus point.

The first reference VRF controller already demonstrates this requirement for set temperature:

```text
integer temperature register
+
separate 0.5 °C flag
=
one semantic SetTemperature value
```

Therefore the production profile schema must not assume:

```text
one semantic field == one Modbus register
```

The profile format should support a small deterministic composite mechanism for cases such as:

- integer part + fractional flag;
- value + validity/status flag;
- low word + high word;
- main value + sign/scale selector when a real device requires it.

A composite point must still expose one semantic value to the rest of MDVWB.

Conceptually:

```json
{
  "setTemperature": {
    "type": "composite_number",
    "readParts": {
      "integer": {
        "space": "holding_register",
        "address": 30
      },
      "halfDegree": {
        "space": "holding_register",
        "address": 36,
        "extract": {
          "mask": 1,
          "shift": 0
        }
      }
    },
    "writeParts": {
      "integer": {
        "space": "holding_register",
        "address": 80
      },
      "halfDegree": {
        "space": "holding_register",
        "address": 84,
        "extract": {
          "mask": 1,
          "shift": 0
        }
      }
    }
  }
}
```

The JSON above is **conceptual**, not the final schema syntax.

The exact composite schema should be finalized while implementing the first real profile. It should remain declarative and deterministic rather than becoming a general-purpose scripting system.

For the first reference equipment, the intended semantic behavior is conceptually:

```text
read:
physical = integer + (halfDegree ? 0.5 : 0.0)

write 22.5:
integer = 22
halfDegree = 1
```

Do not implement this equipment by hardcoding those two registers in the common Modbus engine.

## 19. Numeric representation

Profiles should be able to declare the raw numeric representation when required.

Initial candidates:

```text
uint16
int16
```

Future support may include:

```text
uint32
int32
float32
```

Example:

```json
{
  "rawType": "int16"
}
```

32-bit values introduce word-order questions and should not be guessed. If/when added, the profile must declare byte/word ordering explicitly.

## 20. Rounding and exact writes

The inverse transformation may produce a non-integer raw value.

The implementation must not silently apply arbitrary rounding.

A profile should be able to specify write behavior, for example:

```json
{
  "writeConversion": {
    "rounding": "nearest"
  }
}
```

Potential values:

```text
exact
nearest
floor
ceil
```

Recommended default for normal physical setpoints is `exact` after validating the configured physical `step`.

A profile requiring non-exact rounding must declare it explicitly.

## 21. Limits and step

Writable numeric values may define physical constraints:

```json
{
  "limits": {
    "min": 16.0,
    "max": 30.0,
    "step": 0.5
  }
}
```

These limits are expressed in MDVWB physical units, after scaling.

The engine must reject an invalid command before sending it to the bus.

## 22. Enumerated values

Manufacturer-specific enum values must be mapped explicitly.

Example Mode:

```json
{
  "mode": {
    "type": "enum",
    "read": {
      "space": "holding_register",
      "address": 28
    },
    "write": {
      "space": "holding_register",
      "address": 78
    },
    "map": {
      "0": "cool",
      "1": "heat",
      "2": "dry",
      "3": "fan"
    }
  }
}
```

The common engine returns semantic values such as:

```text
cool
heat
dry
fan
```

It must not expose raw `0`, `1`, `2`, `3` to the rest of MDVWB.

## 23. Different read and write mappings

Some manufacturers may use different codes for status and commands.

The schema should support:

```json
{
  "mode": {
    "type": "enum",
    "readMap": {
      "10": "cool",
      "20": "heat"
    },
    "writeMap": {
      "cool": 1,
      "heat": 2
    }
  }
}
```

A single `map` is a shorthand only when the mapping is safely reversible.

## 24. Power mapping

Power must remain a separate semantic field.

Example:

```json
{
  "power": {
    "type": "enum",
    "map": {
      "0": "off",
      "1": "on"
    }
  }
}
```

Do not encode `off` as a fake Mode value in the common MDVWB model.

## 25. Fan speed mapping

The normal semantic MDVWB fan-speed model should remain:

```text
low
medium
high
auto
```

Example:

```json
{
  "fanSpeed": {
    "type": "enum",
    "readMap": {
      "0": "auto",
      "1": "low",
      "2": "medium",
      "3": "high"
    },
    "writeMap": {
      "auto": 0,
      "low": 1,
      "medium": 2,
      "high": 3
    }
  }
}
```

A device with more native speeds may map several raw read values to one semantic speed and choose a representative raw value for writes.

Example concept:

```text
native 1,2   -> low
native 3,4,5 -> medium
native 6,7   -> high

write low    -> native 1
write medium -> native 4
write high   -> native 7
```

This mapping belongs to the profile.

## 26. Bit fields

Some devices pack multiple states into one register.

The profile format should support a bit or bit-mask extractor.

Conceptual example:

```json
{
  "power": {
    "type": "enum",
    "read": {
      "space": "holding_register",
      "address": 10
    },
    "extract": {
      "mask": 1,
      "shift": 0
    },
    "map": {
      "0": "off",
      "1": "on"
    }
  }
}
```

Another field may use another mask on the same register.

The engine may optimize duplicate reads later, but the semantic profile remains independent of that optimization.

## 27. Alarm/error mapping

Alarm data may be:

- boolean;
- numeric alarm code;
- bit field;
- multiple registers.

The common profile format should support at least boolean and numeric alarm code initially.

Example:

```json
{
  "alarmCode": {
    "type": "number",
    "read": {
      "space": "holding_register",
      "address": 50
    }
  }
}
```

If later equipment requires complex multi-register alarm decoding, that may justify extending the schema or using a specialized adapter.

Do not invent a universal alarm model before real equipment requires it.

## 28. Capabilities

Profiles explicitly declare supported user-facing functions.

Conceptual example:

```json
{
  "capabilities": {
    "power": true,
    "cool": true,
    "heat": true,
    "dry": true,
    "fan": true,
    "autoMode": false,
    "fanLow": true,
    "fanMedium": true,
    "fanHigh": true,
    "autoFan": true,
    "setTemperature": true,
    "roomTemperature": true,
    "alarm": true,
    "blinds": false,
    "lock": false
  }
}
```

The UI should react to capabilities, not to profile IDs.

## 29. Scan probe

Every profile intended for normal MDVWB scanning must define a safe read-only probe.

Conceptual example:

```json
{
  "probe": {
    "point": "power",
    "validation": {
      "responseRequired": true
    }
  }
}
```

For each logical address `1..63`:

1. Resolve logical address to Slave ID and register offset.
2. Build the profile-defined read request.
3. Send it.
4. Validate normal Modbus response.
5. If the profile has additional safe validation, apply it.
6. Mark the logical address online/offline for scan results.

The probe must never perform a write.

## 30. Probe validation

A Modbus response from a Slave ID does not always prove that the expected air-conditioner exists behind a gateway.

Profiles may therefore optionally validate a returned value.

Conceptual example:

```json
{
  "probe": {
    "point": "status",
    "validation": {
      "allowedRawValues": [0, 1, 2, 3, 4]
    }
  }
}
```

Validation should remain conservative.

Do not reject a real device merely because an undocumented but valid operational value appears unless the manufacturer's documentation makes the allowed set reliable.

## 31. Unsupported logical candidates

A profile may know that some logical candidates from `1..63` cannot exist for that equipment.

It should be able to mark such addresses unsupported without sending traffic.

Example concept:

```json
{
  "unsupportedLogicalAddresses": [33, 34, 35]
}
```

MDVWB still conceptually scans the complete `1..63` range, but unsupported candidates are skipped deterministically by the profile.

This should be uncommon. The default is that all `1..63` candidates are resolvable.

## 32. Installation-specific parameters

Some profiles may require a small installation-specific value such as:

- fixed gateway Slave ID;
- controller number;
- register base;
- site-specific offset.

Do not create a new profile file for every installation merely because the gateway Slave ID changed.

The profile format should therefore be able to declare parameters.

Conceptual example:

```json
{
  "parameters": {
    "gatewaySlaveId": {
      "type": "integer",
      "default": 1,
      "min": 1,
      "max": 247
    }
  }
}
```

The bus configuration stores the selected value.

The profile may use that parameter in its resolver.

The expression mechanism for parameters is intentionally not specified yet. It should remain small and deterministic rather than becoming a general scripting language.

## 33. Avoid a general-purpose scripting language

Profiles should describe data, mappings and simple deterministic address formulas.

Avoid embedding JavaScript, Lua, Python or arbitrary expressions in profile files.

Reasons:

- harder validation;
- harder security review;
- harder deterministic testing;
- profiles become executable code;
- malformed profiles can affect runtime behavior far beyond register mapping.

If a device truly requires stateful or complex protocol logic, use a small compiled adapter extension point instead.

## 34. Profile validation requirements

Before opening the bus, the loader should validate the selected profile.

Minimum checks:

- supported `schemaVersion`;
- unique valid `id`;
- `registerAddressing == "pdu_zero_based"`;
- logical range does not exceed `1..63`;
- valid Slave IDs;
- valid PDU register addresses;
- known data spaces;
- valid raw types;
- non-zero scale;
- valid min/max/step;
- enum maps are structurally valid;
- write mapping exists for writable enum commands;
- probe exists;
- probe is read-only;
- all referenced points exist;
- addressing resolver can handle scan candidates;
- explicit mappings do not contain duplicate/invalid logical IDs.

Invalid profiles must not be partially activated.

## 35. Unknown raw values

A profile decoder will eventually encounter an undocumented raw enum value.

The common engine should not reinterpret it as another known state.

Preferred behavior:

- keep the Modbus transaction valid;
- log/diagnose the unknown raw value;
- avoid overwriting the last confirmed semantic value for that field when safe;
- continue processing other independently valid fields.

The exact state-update policy will be defined during implementation.

## 36. Profile loading

Profiles shipped with MDVWB should be discoverable automatically from a profile directory.

Adding a normal new profile should not require adding its ID to a C++ `switch` statement.

Conceptually:

```text
modbus-profiles/
    vendor_a_model_x.json
    vendor_b_gateway_y.json
    vendor_c_fancoil_z.json
```

The loader:

1. finds profile files;
2. validates them;
3. indexes them by `id`;
4. exposes valid profiles to configuration/UI.

Invalid profile files should produce visible diagnostics and must not break unrelated valid profiles.

## 37. Example: conventional direct-Slave profile

Conceptual shortened example:

```json
{
  "schemaVersion": 1,
  "id": "example_direct_fcu",
  "name": "Example Direct Modbus FCU",
  "registerAddressing": "pdu_zero_based",
  "transport": {
    "baudRate": 9600,
    "dataBits": 8,
    "parity": "none",
    "stopBits": 1
  },
  "addressing": {
    "type": "direct_slave",
    "logicalMin": 1,
    "logicalMax": 63,
    "slaveId": {
      "source": "logicalAddress"
    },
    "registerOffset": 0
  },
  "capabilities": {
    "power": true,
    "cool": true,
    "heat": true,
    "dry": true,
    "fan": true,
    "autoMode": false,
    "fanLow": true,
    "fanMedium": true,
    "fanHigh": true,
    "autoFan": true,
    "setTemperature": true,
    "roomTemperature": true,
    "alarm": true
  },
  "probe": {
    "point": "power"
  },
  "points": {
    "power": {
      "type": "enum",
      "read": {
        "space": "holding_register",
        "address": 0
      },
      "write": {
        "space": "holding_register",
        "address": 0
      },
      "map": {
        "0": "off",
        "1": "on"
      }
    },
    "roomTemperature": {
      "type": "number",
      "read": {
        "space": "holding_register",
        "address": 10
      },
      "transform": {
        "scale": 0.1,
        "offset": 0
      },
      "rawType": "int16"
    },
    "setTemperature": {
      "type": "number",
      "read": {
        "space": "holding_register",
        "address": 11
      },
      "write": {
        "space": "holding_register",
        "address": 11
      },
      "transform": {
        "scale": 0.1,
        "offset": 0
      },
      "limits": {
        "min": 16.0,
        "max": 30.0,
        "step": 0.5
      },
      "rawType": "uint16"
    }
  }
}
```

## 38. Example: fixed gateway + repeated register block

Conceptual shortened example:

```json
{
  "schemaVersion": 1,
  "id": "example_gateway_fcu",
  "name": "Example Gateway FCU",
  "registerAddressing": "pdu_zero_based",
  "addressing": {
    "type": "fixed_slave_stride",
    "logicalMin": 1,
    "logicalMax": 63,
    "slaveId": 1,
    "firstLogicalAddress": 1,
    "registerStride": 91
  },
  "probe": {
    "point": "power"
  },
  "points": {
    "power": {
      "type": "enum",
      "read": {
        "space": "holding_register",
        "address": 27,
        "reference": "40028"
      },
      "write": {
        "space": "holding_register",
        "address": 77,
        "reference": "40078"
      },
      "map": {
        "0": "off",
        "1": "on"
      }
    }
  }
}
```

For logical address `2`:

```text
register offset = 91

power read:
27 + 91 = PDU address 118

power write:
77 + 91 = PDU address 168
```

The human-readable manufacturer references may be retained only as comments/metadata.

## 39. What belongs outside the profile

The profile should not contain:

- `/dev/tty...` serial path;
- list of currently enabled/configured logical devices;
- MQTT broker credentials;
- MQTT topic names;
- scheduler configuration;
- dashboard coordinates;
- room names;
- user-facing installation labels;
- live online/offline status.

Those belong to bus/runtime/application configuration.

## 40. What may require a compiled adapter

A profile alone may not be enough when equipment requires:

- stateful multi-step handshakes;
- page/bank switching with command sequences;
- checksums inside manufacturer payloads embedded in registers;
- dynamic register discovery that changes at runtime;
- complex multi-register packed structures not reasonably expressible declaratively;
- protocol behavior beyond normal Modbus transactions.

In that case:

```text
common Modbus RTU engine
        |
specialized profile adapter
        |
device
```

The adapter should remain isolated and expose the same semantic contract as a normal profile.

It must not introduce manufacturer branches throughout the main application.

## 41. First implementation principle

Do not implement every possible field in this document immediately.

The first implementation should support the smallest schema that fully describes the first real supported Modbus equipment.

Then extend the schema only when another real register table requires it.

This prevents the profile format from becoming an imaginary universal PLC language before it has even controlled one fan-coil.

## 42. Acceptance criteria for the profile format

The format is successful when:

1. A direct-Slave device can be described without C++ changes.
2. A fixed-gateway/repeated-register-block device can be described without C++ changes.
3. Scan can resolve every logical candidate `1..63`.
4. Read and write registers can differ.
5. `235` can declaratively decode to `23.5 °C`.
6. `22.5 °C` can declaratively encode to `225`.
7. Mode codes can differ by manufacturer.
8. FanSpeed codes can differ by manufacturer.
9. Capabilities can hide unsupported UI functions.
10. Register notation cannot silently produce a 40001/zero-based off-by-one error.
11. Invalid profiles fail before normal bus polling begins.
12. Adding a conventional new profile does not require modifying the common Modbus engine.
13. One semantic value may be assembled from multiple Modbus points without manufacturer-specific code in the common engine.

## 43. Open items for implementation

The following details remain intentionally open:

- exact JSON Schema file and validation library;
- exact profile directory names;
- bus-level override rules for serial settings;
- exact raw numeric types implemented in version 1;
- exact Modbus write functions implemented in version 1;
- adjacent-register read batching;
- caching duplicate reads when several semantic points share one register;
- exact unknown-enum state-update behavior;
- exact parameter substitution mechanism;
- exact composite-point schema and supported composition operators;
- whether local custom profiles are supported in the first release;
- whether profile reload requires process restart.

These items should be decided while implementing the first real profile and must remain consistent with `ARCHITECTURE.md`.
