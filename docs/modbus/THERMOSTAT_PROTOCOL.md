# Thermostat Modbus RTU profile

> Production profile: `profiles/modbus/thermostat.json`.
>
> Source of register facts: the first block named «Основные регистры работы»
> in the supplied thermostat register table, plus the confirmed transport,
> addressing and function-code details listed below. The second block and
> «Продвинутые настройки» are intentionally outside this profile.
>
> Status: the profile, codec and runtime path are covered by automated tests.
> Live behavior on the target thermostat must still be recorded during the
> Wiren Board smoke test.

## 1. Transport and device addressing

```text
Protocol:       Modbus RTU
Default serial: 9600 8N1
Read function:  FC03 Read Holding Registers
Write function: FC06 Write Single Register
```

Each thermostat has its own Modbus Slave ID. MDVWB uses direct addressing:

```text
logical address 1  -> Slave ID 1
logical address 2  -> Slave ID 2
...
logical address 63 -> Slave ID 63
```

The profile does not add a register offset. Manufacturer addresses are exact
PDU addresses: `0x2060` is sent as decimal address `8288`, not `8287`.

## 2. Active register mapping

All active points use Holding Registers.

| HEX | DEC | Access | Table meaning | MDVWB point and conversion |
|---:|---:|---|---|---|
| `0x2060` | `8288` | R/W | Unit power | `power`: `1=off`, `2=on` |
| `0x2061` | `8289` | R/W | Unit speed | `fanSpeed`: `1=low`, `2=medium`, `3=high`, `4=auto` |
| `0x2062` | `8290` | R/W | Unit mode | `mode`: `1=cool`, `2=heat` |
| `0x2064` | `8292` | R | Integer room temperature | `roomTemperature`: raw value in whole degrees Celsius |
| `0x2065` | `8293` | R/W | Temperature setpoint with one decimal place | `setTemperature`: `physical = raw × 0.1` |

The exposed setpoint range is `16..34 °C` with a user-facing step of `1 °C`.
For example:

```text
read raw 210  -> 21 °C
write 21 °C   -> raw 210
```

Only a matching FC03 read-back makes a command factual. An FC06 response must
echo the requested Slave ID, register address and raw value, but that echo alone
does not update MQTT state.

## 3. Registers intentionally not exposed

The first source block also contains two alternative representations:

| HEX | DEC | Source meaning | Reason not exposed |
|---:|---:|---|---|
| `0x2063` | `8291` | Room temperature with one decimal place | `0x2064` is the selected whole-degree factual value |
| `0x2066` | `8294` | Integer part of the setpoint | It is read-only; `0x2065` is required for confirmed read/write control |

The following source data is excluded completely:

- the second block also named «Основные регистры работы»;
- «Продвинутые настройки»;
- blank/reserved addresses;
- the graph-control register from the second block.

No alarm register is documented in the selected block, so the profile does not
declare `alarmCode`. Auto, dry and fan-only operating modes are also not
invented; only cooling and heating are exposed.

## 4. Discovery

Discovery reads one Holding Register at `0x2060` for each logical address
`1..63`. Any structurally valid FC03 response marks that Slave ID as present.
Discovery never sends FC06 and never changes thermostat settings.

## 5. Automated verification

`mdvwb_modbus_thermostat_profile_test` verifies:

- profile identity, `9600 8N1` and direct Slave addressing;
- literal addresses `0x2060..0x2065` without an offset;
- all Power, Mode and FanSpeed mappings;
- integer room temperature from `0x2064`;
- `21 °C <-> raw 210` and limits `16..34 °C`;
- FC06 request selection by the driver;
- factual confirmation through FC03.

The generic RTU tests separately verify the FC06 frame, CRC, echoed response and
serial-transport acceptance.

## 6. Required live smoke test

Before marking the profile hardware-confirmed, verify on Wiren Board:

1. discovery of every configured Slave ID;
2. factual Power, Mode, Speed, room temperature and setpoint reads;
3. Power off/on through FC06;
4. cooling/heating and all four fan-speed commands;
5. setpoints `16`, `21` and `34 °C`;
6. FC03 read-back after every command;
7. offline and recovery behavior after communication loss.

Record any raw-value difference before changing the profile. Do not compensate
for a device variation in the common Modbus engine.
