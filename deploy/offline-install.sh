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

for required in MDVWB mdvwb-run mdvwb.service mdvwb.env SHA256SUMS; do
    if [ ! -e "$SCRIPT_DIR/$required" ]; then
        echo "Offline package is incomplete: missing $required" >&2
        exit 4
    fi
done

(
    cd "$SCRIPT_DIR"
    sha256sum -c SHA256SUMS
)

WAS_ACTIVE=0
if systemctl is-active --quiet mdvwb.service 2>/dev/null; then
    WAS_ACTIVE=1
    systemctl stop mdvwb.service
fi

install -d -m 0755 /usr/local/bin /usr/local/lib/mdvwb /etc/default /etc/systemd/system
install -m 0755 "$SCRIPT_DIR/MDVWB" /usr/local/bin/MDVWB
install -m 0755 "$SCRIPT_DIR/mdvwb-run" /usr/local/lib/mdvwb/mdvwb-run
install -m 0644 "$SCRIPT_DIR/mdvwb.service" /etc/systemd/system/mdvwb.service

if [ ! -e /etc/default/mdvwb ]; then
    install -m 0640 "$SCRIPT_DIR/mdvwb.env" /etc/default/mdvwb
    echo "Created /etc/default/mdvwb from the safe one-device example."
else
    echo "Kept existing /etc/default/mdvwb unchanged."
fi

systemctl daemon-reload

/usr/local/bin/MDVWB --version
/usr/local/bin/MDVWB --self-test

if [ "$WAS_ACTIVE" -eq 1 ]; then
    systemctl restart mdvwb.service
    echo "MDVWB was upgraded and restarted."
else
    echo "MDVWB installed but not started."
    echo "Edit /etc/default/mdvwb, then run: systemctl enable --now mdvwb.service"
fi

echo "View logs with: journalctl -u mdvwb.service -f"
