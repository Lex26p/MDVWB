# MDVWB release checklist

This checklist is the final verification procedure for an MDVWB release.

## 1. Local Windows verification

Run from `C:\Projects\MDVWB`:

```powershell
cmake --preset x64-debug
cmake --build "out/build/x64-debug"
ctest --test-dir "out/build/x64-debug" -C Debug --output-on-failure
```

The release candidate is not ready while any CTest test is failing.

Node.js is not required for the release procedure.

## 2. GitHub validation

After pushing the release commit:

1. Open **Actions**.
2. Confirm that **Validate MDVWB** completed successfully.
3. Open **Build ARM64 Offline Package**.
4. Run the workflow manually for the release commit.
5. Download the `MDVWB-arm64-offline` artifact.

The validation workflow checks:

- deployment shell syntax;
- default JSON files;
- presence of both web applications and their modules;
- Release configuration and compilation;
- the complete CTest suite;
- binary version, protocol self-test, manager validation and scheduler CLI startup.

## 3. Verify the downloaded archive

The artifact contains:

- `MDVWB-arm64-offline.tar.gz`;
- `MDVWB-arm64-offline.tar.gz.sha256`.

On Linux or Wiren Board:

```sh
sha256sum -c MDVWB-arm64-offline.tar.gz.sha256
tar -xzf MDVWB-arm64-offline.tar.gz
cd MDVWB-arm64
sha256sum -c SHA256SUMS
```

Both checksum commands must report `OK`.

## 4. Offline installation

Copy the extracted `MDVWB-arm64` directory to the controller and run:

```sh
cd MDVWB-arm64
sudo ./offline-install.sh
```

The installer preserves existing files:

- `/etc/mdvwb/buses.json`;
- `/etc/mdvwb/dashboard.json`;
- `/etc/mdvwb/schedules.json`;
- uploaded files under `/var/www/fancoils/assets`.

Default dashboard and schedule configurations are installed only when the
corresponding files do not exist or are empty.

## 5. Controller smoke test

Run:

```sh
systemctl status mdvwb-manager.service --no-pager
systemctl status mdvwb-scheduler.service --no-pager
systemctl list-units "mdvwb@*.service" --no-pager
journalctl -u mdvwb-manager.service -u mdvwb-scheduler.service -n 100 --no-pager
```

Verify in a browser:

- `http://<WB-address>/mdvwb/`;
- `http://<WB-address>/fancoils/`.

Required checks:

- engineering web opens without missing JavaScript modules;
- fan-coil web loads the configured panel;
- MQTT status changes to connected;
- controller time is visible and receives a fresh scheduler heartbeat;
- individual control is confirmed by factual MQTT state;
- group control changes only selected parameters;
- manual schedule execution reaches a terminal result;
- a test automatic schedule executes at the controller's local time;
- stopping scheduler eventually marks its heartbeat as stale and blocks manual run;
- restarting scheduler restores the fresh status automatically.

## 6. Recovery

Keep the previous known-good ARM64 archive until the smoke test is complete.

To restore the previous binaries and static web files, unpack the previous
archive and run its `offline-install.sh` again. Existing MDVWB configuration
files and uploaded dashboard assets remain in place.
