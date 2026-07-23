# MDVWB configuration cycle — step 3

This step adds service synchronization for any number of configured buses.

New manager commands:

```text
mdvwb-manager plan [buses.json]
mdvwb-manager apply [buses.json]
```

`plan` is read-only. It shows which `/etc/default/mdvwb-N` files and
`mdvwb@N.service` instances would be changed.

`apply` must run as root. It:

- atomically writes one `/etc/default/mdvwb-N` environment file for each bus;
- starts and enables new active buses;
- restarts only active buses whose generated configuration changed;
- ensures unchanged active buses are enabled and running;
- stops and disables inactive buses;
- stops and removes buses deleted from `buses.json` only when their environment
  files contain the manager ownership marker.

Environment overrides used by tests and advanced installations:

```text
MDVWB_DEFAULT_DIR
MDVWB_ENV_TEMPLATE
MDVWB_SYSTEMCTL
```

The existing `/usr/local/lib/mdvwb/mdvwb.env` remains the source of common
serial, timing and MQTT defaults. Each generated file overrides only:

```text
MDVWB_BUS
MDVWB_PORT
MDVWB_ADDRESSES
```

This source step is intentionally not installed on the controller yet. It will
be included in the next planned ARM64 checkpoint together with MQTT management.
