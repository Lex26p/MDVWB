# VRF Add Controller production-profile verification checklist

> Purpose: record which source-table uncertainties are resolved for the first production profile and which capabilities remain intentionally disabled.

## Current state

The manufacturer workbook facts remain frozen in:

```text
tests/fixtures/modbus/vrf_add_controller_source.json
```

The executable profile is:

```text
profiles/modbus/vrf_add_controller.json
```

The source fixture remains non-executable even after live verification. It records evidence; the production profile contains actual Modbus addresses and capabilities.

## Resolved item 1: wire register-address convention

### Live evidence

A working WirenBoard installation reads the workbook Power status register `40028` using register address `40028` directly.

Production rule:

```text
workbook source 40028 -> Modbus request address 40028
```

No `40001` or `1` offset is subtracted.

This rule is applied consistently to the profile's confirmed addresses.

## Resolved item 2: safe read-only presence probe

### Live evidence

For an absent indoor unit, the inlet-air temperature point returns raw `0`.

Source point:

```text
40039 + 91*Y
```

Production scan rule:

```text
raw != 0 -> Found
raw == 0 -> NotFound
```

The profile expresses this using:

```json
"presence": "any_nonzero"
```

The scan remains read-only FC03.

### Known limitation

A real indoor unit that legitimately reports raw inlet temperature `0` would be classified as absent. This rule is accepted for the initial profile because it matches the observed installation behavior.

## Initial logical-address policy

The production profile uses:

```text
logical 1 -> Y = 0
logical 2 -> Y = 1
...
logical 63 -> Y = 62
```

with:

```text
Slave ID = 1
registerStride = 91
```

The workbook defines `Y` as a sorted index. A topology change may therefore shift physical-unit identity between logical slots. This is documented as a current limitation rather than hidden behind the resolver.

## Enabled production capabilities

### Power

Confirmed from table and address convention:

```text
read  40028 + 91*Y
write 40078 + 91*Y

0 -> OFF
1 -> ON
```

Enabled.

### AlarmCode

Confirmed from table:

```text
40035 + 91*Y
0 -> no alarm
nonzero -> vendor alarm code
```

Enabled as an unsigned raw numeric code.

## Deferred capability 1: Mode

Source points:

```text
40029 + 91*Y   status
40079 + 91*Y   control
```

Still confirm:

- one-hot versus multi-bit runtime behavior;
- whether one write mask is sufficient;
- whether unused bits must be preserved.

Capability remains disabled.

## Deferred capability 2: FanSpeed

Source points:

```text
40030 + 91*Y   status
40080 + 91*Y   control
```

Still confirm one-hot behavior and review the mapping from six native fixed speeds to MDVWB Low/Medium/High.

Capability remains disabled.

## Deferred capability 3: SetTemperature

Source points:

```text
40031 + 91*Y   integer status
40037 + 91*Y   half-degree status flag
40081 + 91*Y   integer control
40085 + 91*Y   half-degree control flag
```

This requires composite semantic value support plus hardware confirmation of write behavior.

Capability remains disabled.

## Deferred capability 4: RoomTemperature

Source point:

```text
40039 + 91*Y   inlet air temperature Ti
```

The raw value is already used for scan presence, but scale, offset, signedness and sensor-error sentinel values are still unknown.

Capability remains disabled until physical-unit conversion is verified.

## Promotion rule for future capabilities

A new semantic capability may be enabled only when its raw behavior and conversion are supported by the workbook plus live evidence or equivalent trustworthy documentation.

Unresolved capabilities remain disabled rather than guessed.
