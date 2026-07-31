# VRF Add Controller production-profile verification checklist

> Purpose: define the minimum evidence required before the first reference controller is promoted from source facts to an executable MDVWB Modbus profile.

## Current state

The manufacturer workbook has been re-read and its first-profile facts are frozen in:

```text
tests/fixtures/modbus/vrf_add_controller_source.json
```

That fixture is intentionally non-executable.

## Blocking item 1: wire register-address convention

Required evidence: at least one known-good Modbus RTU request for a workbook point.

Preferred points:

```text
4998
40026 + 91*Y
40028 + 91*Y
```

Record:

```text
Slave ID
function code
starting-address high byte
starting-address low byte
quantity
CRC
which workbook source reference the request targets
```

This decides whether source values such as `40028` are literal PDU addresses or documentation references requiring conversion.

Do not infer the conversion from the number's appearance.

## Blocking item 2: logical identity and safe probe

Confirm whether `Y` remains stable after converter rediscovery/topology changes.

For at least one present and one absent candidate, record what the two identity points return:

```text
40026 + 91*Y   system number
40027 + 91*Y   indoor-unit address
```

A production scan probe must be read-only and must distinguish a real unit from an unused register block without relying on undocumented behavior.

## Blocking item 3: Mode

Capture status and a controlled write for at least two modes.

Source points:

```text
40029 + 91*Y   status
40079 + 91*Y   control
```

Confirm:

- whether values are one-hot bit masks;
- whether exactly one bit is normally set;
- whether writing a single mask is sufficient;
- whether unused bits must be preserved.

## Blocking item 4: FanSpeed

Source points:

```text
40030 + 91*Y   status
40080 + 91*Y   control
```

Confirm one-hot behavior and decide the reviewed mapping from six native fixed speeds to MDVWB's current Low/Medium/High semantic model.

## Blocking item 5: SetTemperature

Source points:

```text
40031 + 91*Y   integer status
40037 + 91*Y   half-degree status flag
40081 + 91*Y   integer control
40085 + 91*Y   half-degree control flag
```

Confirm at minimum `22.0` and `22.5` reads and writes, including whether the two write registers must be sent in one FC10 transaction.

## Blocking item 6: RoomTemperature

Source point:

```text
40039 + 91*Y   inlet air temperature Ti
```

Confirm scale, offset, signedness and any invalid/sensor-error sentinel values.

## Promotion rule

Only facts backed by the workbook plus the verification evidence above may enter the production JSON profile. Unresolved capabilities remain disabled rather than guessed.
