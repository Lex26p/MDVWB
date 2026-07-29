#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OPERATION=install

if [ "$#" -gt 0 ]; then
    case "$1" in
        install|update)
            OPERATION=$1
            shift
            ;;
        --help|-h)
            printf '%s\n' \
                "Usage: ./offline-install.sh [install|update]" \
                "       [--force] [--allow-downgrade] [--dry-run]" \
                "       [--backup-dir <directory>] [--no-backup]"
            exit 0
            ;;
    esac
fi

set -- "$OPERATION" --package "$SCRIPT_DIR" --method offline "$@"
exec sh "$SCRIPT_DIR/mdvwb-setup" "$@"
