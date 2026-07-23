# Configuration cycle — step 4

Adds the MQTT configuration endpoint to `mdvwb-manager`.

Topics:

- `/mdvwb/config` — canonical current JSON, retained;
- `/mdvwb/config/set` — non-retained JSON write command;
- `/mdvwb/config/result` — non-retained operation result;
- `/mdvwb/status` — retained manager state.

The new `mdvwb-manager mqtt [buses.json]` command is intended to run as root on
Wiren Board. It validates every payload, writes the canonical configuration
atomically and synchronizes the `mdvwb@N.service` instances. Retained command
messages are deliberately ignored, so an old write request cannot be replayed
after reconnect or restart.

This development step is tested locally only. No controller installation or
GitHub Actions build is required yet.
