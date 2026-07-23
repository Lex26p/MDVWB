#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root." >&2
    exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_FILE="$SCRIPT_DIR/mdvwb-service-control.js"
TARGET_FILE="/etc/wb-rules/mdvwb-service-control.js"

if [ ! -f "$SOURCE_FILE" ]; then
    echo "Missing $SOURCE_FILE" >&2
    exit 2
fi

install -d -m 0755 /etc/wb-rules
install -m 0644 "$SOURCE_FILE" "$TARGET_FILE"
systemctl restart wb-rules.service

if ! systemctl is-active --quiet wb-rules.service; then
    echo "wb-rules failed to restart." >&2
    systemctl status wb-rules.service --no-pager >&2 || true
    exit 3
fi

echo "Installed $TARGET_FILE"
echo "Virtual device: MDVWB-Service-1"
echo "Open the Wiren Board web interface and refresh the Devices page."
