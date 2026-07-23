#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root." >&2
    exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
EXPECTED_ARCH="arm64"
ACTUAL_ARCH=$(dpkg --print-architecture 2>/dev/null || true)

if [ "$ACTUAL_ARCH" != "$EXPECTED_ARCH" ]; then
    echo "Unsupported architecture: $ACTUAL_ARCH. Expected: $EXPECTED_ARCH." >&2
    exit 2
fi

if ! ldconfig -p 2>/dev/null | grep -q 'libmosquitto\.so\.1'; then
    echo "libmosquitto.so.1 is not installed on this controller." >&2
    exit 3
fi

for required in MDVWB mdvwb-run mdvwb-bus 'mdvwb@.service' mdvwb.env mdvwb-service-control.js SHA256SUMS; do
    if [ ! -e "$SCRIPT_DIR/$required" ]; then
        echo "Offline package is incomplete: missing $required" >&2
        exit 4
    fi
done

(
    cd "$SCRIPT_DIR"
    sha256sum -c SHA256SUMS
)

BUS1_ACTIVE=0
BUS2_ACTIVE=0
if systemctl is-active --quiet mdvwb@1.service 2>/dev/null || systemctl is-active --quiet mdvwb.service 2>/dev/null; then
    BUS1_ACTIVE=1
fi
if systemctl is-active --quiet mdvwb@2.service 2>/dev/null || systemctl is-active --quiet mdvwb-2.service 2>/dev/null; then
    BUS2_ACTIVE=1
fi

systemctl stop mdvwb.service mdvwb-2.service mdvwb@1.service mdvwb@2.service 2>/dev/null || true
systemctl disable mdvwb.service mdvwb-2.service 2>/dev/null || true
rm -f /etc/systemd/system/mdvwb.service /etc/systemd/system/mdvwb-2.service

install -d -m 0755 /usr/local/bin /usr/local/lib/mdvwb /etc/default /etc/systemd/system /etc/wb-rules
install -m 0755 "$SCRIPT_DIR/MDVWB" /usr/local/bin/MDVWB
install -m 0755 "$SCRIPT_DIR/mdvwb-run" /usr/local/lib/mdvwb/mdvwb-run
install -m 0755 "$SCRIPT_DIR/mdvwb-bus" /usr/local/bin/mdvwb-bus
install -m 0640 "$SCRIPT_DIR/mdvwb.env" /usr/local/lib/mdvwb/mdvwb.env
install -m 0644 "$SCRIPT_DIR/mdvwb@.service" /etc/systemd/system/mdvwb@.service
install -m 0644 "$SCRIPT_DIR/mdvwb-service-control.js" /etc/wb-rules/mdvwb-service-control.js

if [ ! -e /etc/default/mdvwb-1 ]; then
    if [ -r /etc/default/mdvwb ]; then
        install -m 0640 /etc/default/mdvwb /etc/default/mdvwb-1
        echo "Migrated /etc/default/mdvwb to /etc/default/mdvwb-1."
    else
        install -m 0640 "$SCRIPT_DIR/mdvwb.env" /etc/default/mdvwb-1
    fi
fi
/usr/local/bin/mdvwb-bus init 1 /dev/ttyRS485-1

if [ -r /etc/default/mdvwb-2 ]; then
    /usr/local/bin/mdvwb-bus init 2 /dev/ttyRS485-2
fi

systemctl daemon-reload
/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test

if [ "$BUS1_ACTIVE" -eq 1 ]; then
    systemctl enable --now mdvwb@1.service
fi
if [ "$BUS2_ACTIVE" -eq 1 ] && [ -r /etc/default/mdvwb-2 ]; then
    systemctl enable --now mdvwb@2.service
fi

systemctl restart wb-rules.service
if ! systemctl is-active --quiet wb-rules.service; then
    echo "wb-rules failed to restart after installing service controls." >&2
    exit 5
fi

echo "Installed unified service template: mdvwb@<bus>.service"
echo "Bus 1: mdvwb-bus show 1"
if [ -r /etc/default/mdvwb-2 ]; then
    echo "Bus 2: mdvwb-bus show 2"
else
    echo "Create bus 2 with: mdvwb-bus init 2 /dev/ttyRS485-2"
fi
