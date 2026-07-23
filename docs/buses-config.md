# MDVWB buses.json

`buses.json` is the future single source of truth for all MDV RS-485 buses.
This step only defines and validates the format. It does not change running services.

```json
{
  "version": 1,
  "buses": [
    {
      "id": 1,
      "enabled": true,
      "port": "/dev/ttyRS485-1",
      "addresses": [1, 2, 3]
    }
  ]
}
```

Rules:

- `version` must be `1`;
- bus `id` must be unique and in range `1..999`;
- `port` must be a unique safe absolute path beginning with `/dev/`;
- addresses must be unique integers in range `0..63`;
- an enabled bus must contain at least one address;
- a disabled bus may have an empty address list;
- unknown JSON fields are rejected to expose configuration typos;
- parsed buses and addresses are normalized into ascending order.

The production path planned for a later step is `/etc/mdvwb/buses.json`.

## Read-only manager commands

Step 2 adds the `mdvwb-manager` command. It is read-only at this stage and does
not start, stop or modify any service.

```sh
mdvwb-manager validate /path/to/buses.json
mdvwb-manager show /path/to/buses.json
mdvwb-manager summary /path/to/buses.json
```

If the path is omitted, the manager uses `MDVWB_BUSES_CONFIG` when that
environment variable is set, otherwise `/etc/mdvwb/buses.json`.

Stable exit codes:

- `0` — command completed successfully;
- `2` — invalid configuration or command-line usage;
- `1` — unexpected manager failure.
