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
ctest --test-dir "$BUILD_DIR" --output-on-failure
cmake --install "$BUILD_DIR"

install -d -m 0755 /usr/local/lib/mdvwb /etc/default /etc/systemd/system
install -m 0755 "$SCRIPT_DIR/mdvwb-run" /usr/local/lib/mdvwb/mdvwb-run
install -m 0644 "$SCRIPT_DIR/mdvwb.service" /etc/systemd/system/mdvwb.service

if [ ! -e /etc/default/mdvwb ]; then
    install -m 0640 "$SCRIPT_DIR/mdvwb.env" /etc/default/mdvwb
    echo "Created /etc/default/mdvwb from the safe one-device example."
else
    echo "Kept existing /etc/default/mdvwb unchanged."
fi

systemctl daemon-reload

echo "MDVWB installed but not started."
echo "Edit /etc/default/mdvwb, then run: systemctl enable --now mdvwb"
echo "View logs with: journalctl -u mdvwb -f"
