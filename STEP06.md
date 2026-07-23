# MDVWB configuration cycle — step 6 of 10

This step adds MQTT-controlled discovery for any configured bus.

## Topics

- Command: `/mdvwb/buses/<id>/discovery/start` (must not be retained)
- Status: `/mdvwb/buses/<id>/discovery/status` (retained)
- Result: `/mdvwb/buses/<id>/discovery/result` (retained)

## Behaviour

1. Load the selected bus from `buses.json`.
2. Publish discovery state `running`.
3. Stop only `mdvwb@<id>.service` when it is active.
4. Run the existing `MDVWB --discover` mode on that bus port.
5. Publish sorted discovered addresses.
6. Leave the selected service stopped.
7. Never modify `buses.json` or apply discovered addresses automatically.

The discovery binary path defaults to `/usr/local/bin/MDVWB` and can be overridden
with `MDVWB_BINARY`.
