# Modbus real-hardware validation

This runbook records the staged acceptance process for Milestone 12. It does not
change the profile from observations that have not yet been reviewed.

## Safety boundary

The read-only stage may perform only:

- manager discovery using the selected profile's reviewed read-only probe;
- normal runtime polling of profile-declared readable points;
- service start/stop needed to preserve the original service state;
- MQTT state and journal capture.

It must not publish device-control topics, run experimental writes, probe an
unknown profile, or read across undeclared register gaps.

## Stage 1 collector

Run on the target controller as root, with the Modbus bus already configured:

```sh
sudo sh tools/modbus-hardware-readonly-check.sh \
  --bus 2 \
  --output /var/tmp/mdvwb-m12-readonly-bus2 \
  --capture-seconds 30
```

The collector triggers one safe discovery pass, temporarily starts the normal
runtime to capture factual states, and restores the bus service to its original
active/inactive state.

The evidence directory contains:

- `manifest.txt`, sanitized configuration metadata without MQTT credentials;
- `discovery.stream.txt`, including the fresh running/result sequence;
- `discovery.result.json` and `discovered-addresses.txt`;
- `mqtt-state.txt`, factual retained states observed from the normal runtime;
- service status and journal excerpts;
- `SUMMARY.txt`, the compact acceptance summary.

A successful collector run proves only that the read-only validation path
completed and every discovered address published Power and AlarmCode during the
capture window. It does not prove Power write behavior, long-term stability, or
recovery after a physical disconnect.

## Milestone 12 stages

1. Add and verify the read-only evidence collector.
2. Run discovery and repeated factual polling on real equipment; review the
   evidence before accepting any new hardware facts.
3. Perform a controlled Power round trip with an operator-observed physical
   result and matching FC03 read-back. No other control is eligible yet.
4. Validate extended polling, physical disconnect/reconnect, and multiple
   configured devices, then record the reviewed evidence in `STATUS.md`.

Mode, FanSpeed, SetTemperature, RoomTemperature and any additional alarm meaning
remain unsupported until their own manufacturer-confirmed hardware evidence is
reviewed in a later step.
