#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root." >&2
    exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${MDVWB_BUILD_DIR:-$SOURCE_DIR/out/build/wirenboard-release}
WWW_ROOT=${MDVWB_WWW_ROOT:-/var/www}

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake libmosquitto-dev

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMDVWB_REQUIRE_MOSQUITTO=ON
cmake --build "$BUILD_DIR" --parallel 1
(cd "$BUILD_DIR" && ctest --output-on-failure)

install -d -m 0755 \
    /usr/local/bin /usr/local/lib/mdvwb /etc/mdvwb /etc/default \
    /etc/systemd/system /var/lib/mdvwb "$WWW_ROOT/mdvwb"
install -m 0755 "$BUILD_DIR/MDVWB" /usr/local/bin/MDVWB
install -m 0755 "$BUILD_DIR/mdvwb-offline" /usr/local/bin/mdvwb-offline
install -m 0755 "$BUILD_DIR/mdvwb-manager" /usr/local/bin/mdvwb-manager
install -m 0755 "$BUILD_DIR/mdvwb-scheduler" /usr/local/bin/mdvwb-scheduler
install -m 0755 "$SCRIPT_DIR/mdvwb-run" /usr/local/lib/mdvwb/mdvwb-run
install -m 0640 "$SCRIPT_DIR/mdvwb.env" /usr/local/lib/mdvwb/mdvwb.env
install -m 0644 "$SCRIPT_DIR/mdvwb@.service" /etc/systemd/system/mdvwb@.service
install -m 0644 "$SCRIPT_DIR/mdvwb-manager.service" /etc/systemd/system/mdvwb-manager.service
install -m 0644 "$SCRIPT_DIR/mdvwb-scheduler.service" /etc/systemd/system/mdvwb-scheduler.service
[ -e /etc/default/mdvwb-manager ] || \
    install -m 0640 "$SCRIPT_DIR/mdvwb-manager.env" /etc/default/mdvwb-manager
[ -e /etc/default/mdvwb-scheduler ] || \
    install -m 0640 "$SCRIPT_DIR/mdvwb-scheduler.env" /etc/default/mdvwb-scheduler
cp -a "$SOURCE_DIR/www/mdvwb/." "$WWW_ROOT/mdvwb/"

# A fresh controller starts with an empty bus configuration. Existing malformed
# legacy files are reported and stop installation instead of being replaced by
# an example containing real ports and addresses.
if [ ! -s /etc/mdvwb/buses.json ]; then
    if /usr/local/bin/mdvwb-manager migrate-defaults /etc/mdvwb/buses.json; then
        echo "Migrated legacy MDVWB bus configuration."
    else
        MIGRATION_STATUS=$?
        if [ "$MIGRATION_STATUS" -eq 3 ]; then
            install -m 0640 "$SCRIPT_DIR/buses.default.json" /etc/mdvwb/buses.json
            echo "No legacy configuration was found; installed an empty buses.json."
        else
            echo "Legacy MDVWB configuration exists but could not be migrated." >&2
            echo "Fix the reported legacy configuration error and run the installer again." >&2
            exit "$MIGRATION_STATUS"
        fi
    fi
fi

systemctl disable --now mdvwb.service mdvwb-2.service 2>/dev/null || true
rm -f /etc/systemd/system/mdvwb.service /etc/systemd/system/mdvwb-2.service
rm -f /usr/local/bin/mdvwb-bus
rm -f /etc/wb-rules/mdvwb-service-control.js
systemctl daemon-reload
/usr/local/bin/mdvwb-manager apply /etc/mdvwb/buses.json
systemctl enable --now mdvwb-manager.service
systemctl enable --now mdvwb-scheduler.service

echo "Installed MDVWB manager, scheduler and web page: http://<WB-address>/mdvwb/"
