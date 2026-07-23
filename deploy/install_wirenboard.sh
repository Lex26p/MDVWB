#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root." >&2
    exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${MDVWB_BUILD_DIR:-$SOURCE_DIR/out/build/wirenboard-release}
INSTALL_PREFIX=${MDVWB_INSTALL_PREFIX:-/usr/local}

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake libmosquitto-dev

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_PREFIX" \
    -DMDVWB_REQUIRE_MOSQUITTO=ON
cmake --build "$BUILD_DIR" --parallel 1
(cd "$BUILD_DIR" && ctest --output-on-failure)
cmake --install "$BUILD_DIR"

install -d -m 0755 /usr/local/bin /usr/local/lib/mdvwb /etc/default /etc/systemd/system /etc/wb-rules
install -m 0755 "$SCRIPT_DIR/mdvwb-run" /usr/local/lib/mdvwb/mdvwb-run
install -m 0755 "$SCRIPT_DIR/mdvwb-bus" /usr/local/bin/mdvwb-bus
install -m 0640 "$SCRIPT_DIR/mdvwb.env" /usr/local/lib/mdvwb/mdvwb.env
install -m 0644 "$SCRIPT_DIR/mdvwb@.service" /etc/systemd/system/mdvwb@.service
install -m 0644 "$SCRIPT_DIR/mdvwb-service-control.js" /etc/wb-rules/mdvwb-service-control.js

if [ ! -e /etc/default/mdvwb-1 ]; then
    if [ -r /etc/default/mdvwb ]; then
        install -m 0640 /etc/default/mdvwb /etc/default/mdvwb-1
    else
        install -m 0640 "$SCRIPT_DIR/mdvwb.env" /etc/default/mdvwb-1
    fi
fi
/usr/local/bin/mdvwb-bus init 1 /dev/ttyRS485-1

systemctl disable --now mdvwb.service 2>/dev/null || true
rm -f /etc/systemd/system/mdvwb.service /etc/systemd/system/mdvwb-2.service
systemctl daemon-reload
systemctl restart wb-rules.service

echo "Installed unified service template: mdvwb@<bus>.service"
echo "Start bus 1 with: mdvwb-bus enable 1"
echo "Create bus 2 with: mdvwb-bus init 2 /dev/ttyRS485-2"
echo "View bus 1 logs with: journalctl -u mdvwb@1.service -f"
