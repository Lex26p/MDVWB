# MDVWB configuration cycle — step 5 of 10

This step adds MQTT control and retained runtime status for every configured bus.

## Command topics

```text
/mdvwb/buses/<id>/start
/mdvwb/buses/<id>/stop
/mdvwb/buses/<id>/restart
/mdvwb/buses/<id>/status/get
```

Commands must be non-retained. The payload is currently ignored; the web UI may publish `1`.

Start and restart are rejected when the selected bus has `enabled=false` in
`buses.json`. Stop and status remain available for every configured bus.

## Published topics

```text
/mdvwb/buses/<id>/status
/mdvwb/buses/<id>/result
```

`status` is retained and contains the configured port, addresses, desired
`enabled` value, actual service activity, and systemd autostart state.

Example:

```json
{
  "id": 3,
  "configured": true,
  "enabled": true,
  "service": "active",
  "autostart": true,
  "port": "/dev/ttyUSB0",
  "addresses": [2, 7, 12]
}
```

`result` is not retained and reports the result of the latest explicit command.

## Safety rules

- retained control commands are ignored;
- bus IDs must be in range 1..999;
- commands are accepted only for buses present in `buses.json`;
- start/restart do not enable a bus disabled in configuration;
- manual start/stop/restart do not change `buses.json` or systemd autostart;
- every bus continues to run in its own `mdvwb@N.service` process.
