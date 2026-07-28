#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root." >&2
    exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
EXPECTED_ARCH="arm64"
ACTUAL_ARCH=$(dpkg --print-architecture 2>/dev/null || true)
WWW_ROOT=${MDVWB_WWW_ROOT:-/var/www}

if [ "$ACTUAL_ARCH" != "$EXPECTED_ARCH" ]; then
    echo "Unsupported architecture: $ACTUAL_ARCH. Expected: $EXPECTED_ARCH." >&2
    exit 2
fi

if ! ldconfig -p 2>/dev/null | grep -q 'libmosquitto\.so\.1'; then
    echo "libmosquitto.so.1 is not installed on this controller." >&2
    exit 3
fi

for required in \
    MDVWB mdvwb-offline mdvwb-manager mdvwb-scheduler mdvwb-run \
    'mdvwb@.service' mdvwb.env mdvwb-manager.service mdvwb-manager.env \
    mdvwb-scheduler.service mdvwb-scheduler.env buses.example.json \
    dashboard.default.json schedules.default.json \
    www/mdvwb/index.html www/mdvwb/app.js www/mdvwb/model.js \
    www/mdvwb/mqtt-client.js www/mdvwb/styles.css \
    www/mdvwb/dashboard-editor.js www/mdvwb/dashboard-model.js \
    www/mdvwb/dashboard-placement-editor.js \
    www/fancoils/index.html www/fancoils/app.js www/fancoils/model.js \
    www/fancoils/schedule-model.js www/fancoils/scheduler-status-ui.js \
    www/fancoils/styles.css SHA256SUMS; do
    if [ ! -e "$SCRIPT_DIR/$required" ]; then
        echo "Offline package is incomplete: missing $required" >&2
        exit 4
    fi
done

(
    cd "$SCRIPT_DIR"
    sha256sum -c SHA256SUMS
)

systemctl stop mdvwb-scheduler.service 2>/dev/null || true
systemctl stop mdvwb-manager.service 2>/dev/null || true

install -d -m 0755 \
    /usr/local/bin /usr/local/lib/mdvwb /etc/mdvwb /etc/default \
    /etc/systemd/system /var/lib/mdvwb \
    "$WWW_ROOT/mdvwb" "$WWW_ROOT/fancoils" "$WWW_ROOT/fancoils/assets"
install -m 0755 "$SCRIPT_DIR/MDVWB" /usr/local/bin/MDVWB
install -m 0755 "$SCRIPT_DIR/mdvwb-offline" /usr/local/bin/mdvwb-offline
install -m 0755 "$SCRIPT_DIR/mdvwb-manager" /usr/local/bin/mdvwb-manager
install -m 0755 "$SCRIPT_DIR/mdvwb-scheduler" /usr/local/bin/mdvwb-scheduler
install -m 0755 "$SCRIPT_DIR/mdvwb-run" /usr/local/lib/mdvwb/mdvwb-run
install -m 0640 "$SCRIPT_DIR/mdvwb.env" /usr/local/lib/mdvwb/mdvwb.env
install -m 0644 "$SCRIPT_DIR/mdvwb@.service" /etc/systemd/system/mdvwb@.service
install -m 0644 "$SCRIPT_DIR/mdvwb-manager.service" /etc/systemd/system/mdvwb-manager.service
install -m 0644 "$SCRIPT_DIR/mdvwb-scheduler.service" /etc/systemd/system/mdvwb-scheduler.service
if [ ! -e /etc/default/mdvwb-manager ]; then
    install -m 0640 "$SCRIPT_DIR/mdvwb-manager.env" /etc/default/mdvwb-manager
fi
if [ ! -e /etc/default/mdvwb-scheduler ]; then
    install -m 0640 "$SCRIPT_DIR/mdvwb-scheduler.env" /etc/default/mdvwb-scheduler
fi

# Copy complete static applications. Existing uploaded backgrounds under
# /var/www/fancoils/assets are not deleted during an update.
cp -R "$SCRIPT_DIR/www/mdvwb/." "$WWW_ROOT/mdvwb/"
cp -R "$SCRIPT_DIR/www/fancoils/." "$WWW_ROOT/fancoils/"
find "$WWW_ROOT/mdvwb" "$WWW_ROOT/fancoils" -type d -exec chmod 0755 {} +
find "$WWW_ROOT/mdvwb" "$WWW_ROOT/fancoils" -type f -exec chmod 0644 {} +

# Build the first buses.json from the currently working per-bus files. Existing
# buses.json is never overwritten by an update.
if [ ! -s /etc/mdvwb/buses.json ]; then
    if ! /usr/local/bin/mdvwb-manager migrate-defaults /etc/mdvwb/buses.json; then
        install -m 0640 "$SCRIPT_DIR/buses.example.json" /etc/mdvwb/buses.json
        echo "No legacy configuration was found; installed buses.example.json." >&2
    fi
fi

# Dashboard and schedule defaults contain no device references, so a new
# installation starts safely even before buses and panels are configured.
if [ ! -s /etc/mdvwb/dashboard.json ]; then
    install -m 0640 "$SCRIPT_DIR/dashboard.default.json" /etc/mdvwb/dashboard.json
fi
if [ ! -s /etc/mdvwb/schedules.json ]; then
    install -m 0640 "$SCRIPT_DIR/schedules.default.json" /etc/mdvwb/schedules.json
fi

/usr/local/bin/mdvwb-manager validate /etc/mdvwb/buses.json

# Remember configured bus ids, stop them, then clean old retained virtual-device
# topics before the new drivers publish their own metadata and states.
BUS_IDS=$(/usr/local/bin/mdvwb-manager summary /etc/mdvwb/buses.json |
    sed -n 's/^bus=\([0-9][0-9]*\) .*/\1/p')
for BUS in $BUS_IDS; do
    systemctl stop "mdvwb@$BUS.service" 2>/dev/null || true
done
systemctl disable --now mdvwb.service mdvwb-2.service 2>/dev/null || true
rm -f /etc/systemd/system/mdvwb.service /etc/systemd/system/mdvwb-2.service
rm -f /usr/local/bin/mdvwb-bus

# Disable only the legacy rule that hard-codes ArrID and creates Fan-* devices.
# A timestamp-free backup remains next to the original file.
for RULE in /etc/wb-rules/*.js; do
    [ -f "$RULE" ] || continue
    if grep -Eq 'var[[:space:]]+ArrID[[:space:]]*=' "$RULE" &&
       grep -q 'defineVirtualDevice("Fan-"' "$RULE"; then
        mv "$RULE" "$RULE.disabled-mdvwb"
        echo "Disabled legacy fan virtual-device rule: $RULE"
    fi
done
rm -f /etc/wb-rules/mdvwb-service-control.js
systemctl restart wb-rules.service 2>/dev/null || true

if command -v mqtt-delete-retained >/dev/null 2>&1; then
    for BUS in $BUS_IDS; do
        ADDRESS=0
        while [ "$ADDRESS" -le 63 ]; do
            mqtt-delete-retained "/devices/Fan-${BUS}_${ADDRESS}/#" >/dev/null 2>&1 || true
            ADDRESS=$((ADDRESS + 1))
        done
        mqtt-delete-retained "/devices/sist-${BUS}/#" >/dev/null 2>&1 || true
    done
fi

systemctl daemon-reload
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test
/usr/local/bin/mdvwb-offline --self-test
/usr/local/bin/mdvwb-manager apply /etc/mdvwb/buses.json
systemctl enable --now mdvwb-manager.service
systemctl enable --now mdvwb-scheduler.service

sleep 3
if ! systemctl is-active --quiet mdvwb-manager.service; then
    echo "mdvwb-manager.service failed to start." >&2
    systemctl status mdvwb-manager.service --no-pager >&2 || true
    exit 5
fi
if ! systemctl is-active --quiet mdvwb-scheduler.service; then
    echo "mdvwb-scheduler.service failed to start." >&2
    systemctl status mdvwb-scheduler.service --no-pager >&2 || true
    exit 6
fi

printf '%s\n' "Installed MDVWB multi-bus manager and scheduler."
printf '%s\n' "Configuration: /etc/mdvwb/buses.json"
printf '%s\n' "Dashboard: /etc/mdvwb/dashboard.json"
printf '%s\n' "Schedules: /etc/mdvwb/schedules.json"
printf '%s\n' "Engineering web: http://<WB-address>/mdvwb/"
printf '%s\n' "Fan-coil web: http://<WB-address>/fancoils/"
printf '%s\n' "Manager: systemctl status mdvwb-manager.service --no-pager"
printf '%s\n' "Scheduler: systemctl status mdvwb-scheduler.service --no-pager"
