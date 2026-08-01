#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Run this installer as root." >&2
    exit 1
fi

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SOURCE_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BUILD_DIR=${MDVWB_BUILD_DIR:-$SOURCE_DIR/out/build/wirenboard-release}
OPERATION=install
FORCE=0
ALLOW_DOWNGRADE=0
DRY_RUN=0
NO_BACKUP=0
BACKUP_DIR=

if [ "$#" -gt 0 ]; then
    case "$1" in
        install|update)
            OPERATION=$1
            shift
            ;;
    esac
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --force)
            FORCE=1
            shift
            ;;
        --allow-downgrade)
            ALLOW_DOWNGRADE=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --no-backup)
            NO_BACKUP=1
            shift
            ;;
        --backup-dir)
            [ "$#" -ge 2 ] || {
                echo "--backup-dir requires a directory." >&2
                exit 2
            }
            BACKUP_DIR=$2
            shift 2
            ;;
        --help|-h)
            printf '%s\n' \
                "Usage: ./deploy/install-from-source.sh [install|update]" \
                "       [--force] [--allow-downgrade] [--dry-run]" \
                "       [--backup-dir <directory>] [--no-backup]"
            exit 0
            ;;
        *)
            echo "Unknown installer argument: $1" >&2
            exit 2
            ;;
    esac
done

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y \
    build-essential cmake libmosquitto-dev

cmake -S "$SOURCE_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMDVWB_REQUIRE_MOSQUITTO=ON
cmake --build "$BUILD_DIR" --parallel 1
ctest --test-dir "$BUILD_DIR" --output-on-failure

PROJECT_VERSION=$(sed -n \
    's/^project(MDVWB VERSION \([0-9][0-9.]*\) LANGUAGES CXX)$/\1/p' \
    "$SOURCE_DIR/CMakeLists.txt")
[ -n "$PROJECT_VERSION" ] || {
    echo "Cannot determine MDVWB project version." >&2
    exit 2
}
PROJECT_COMMIT=$(git -C "$SOURCE_DIR" rev-parse HEAD 2>/dev/null ||
    printf '%s' unknown)
BUILT_AT=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

STAGING=${TMPDIR:-/tmp}/mdvwb-source-package.$$
trap 'rm -rf "$STAGING"' EXIT HUP INT TERM
mkdir -p "$STAGING/www" "$STAGING/modbus-profiles"

install -m 0755 "$BUILD_DIR/MDVWB" "$STAGING/MDVWB"
install -m 0755 "$BUILD_DIR/mdvwb-offline" "$STAGING/mdvwb-offline"
install -m 0755 "$BUILD_DIR/mdvwb-manager" "$STAGING/mdvwb-manager"
install -m 0755 "$BUILD_DIR/mdvwb-scheduler" "$STAGING/mdvwb-scheduler"
install -m 0755 "$BUILD_DIR/mdvwb-modbus" "$STAGING/mdvwb-modbus"
install -m 0644 "$SOURCE_DIR/profiles/modbus/vrf_add_controller.json" \
    "$STAGING/modbus-profiles/vrf_add_controller.json"
install -m 0755 "$SCRIPT_DIR/mdvwb-run" "$STAGING/mdvwb-run"
install -m 0755 "$SCRIPT_DIR/mdvwb-setup" "$STAGING/mdvwb-setup"
install -m 0755 "$SCRIPT_DIR/offline-install.sh" "$STAGING/offline-install.sh"
install -m 0644 "$SCRIPT_DIR/mdvwb@.service" "$STAGING/mdvwb@.service"
install -m 0640 "$SCRIPT_DIR/mdvwb.env" "$STAGING/mdvwb.env"
install -m 0644 "$SCRIPT_DIR/mdvwb-manager.service" \
    "$STAGING/mdvwb-manager.service"
install -m 0640 "$SCRIPT_DIR/mdvwb-manager.env" \
    "$STAGING/mdvwb-manager.env"
install -m 0644 "$SCRIPT_DIR/mdvwb-scheduler.service" \
    "$STAGING/mdvwb-scheduler.service"
install -m 0640 "$SCRIPT_DIR/mdvwb-scheduler.env" \
    "$STAGING/mdvwb-scheduler.env"
install -m 0640 "$SCRIPT_DIR/buses.default.json" \
    "$STAGING/buses.default.json"
install -m 0640 "$SCRIPT_DIR/dashboard.default.json" \
    "$STAGING/dashboard.default.json"
install -m 0640 "$SCRIPT_DIR/schedules.default.json" \
    "$STAGING/schedules.default.json"
cp -a "$SOURCE_DIR/www/mdvwb" "$STAGING/www/mdvwb"
cp -a "$SOURCE_DIR/www/fancoils" "$STAGING/www/fancoils"

sed \
    -e "s/@MDVWB_VERSION@/$PROJECT_VERSION/g" \
    -e "s/@MDVWB_COMMIT@/$PROJECT_COMMIT/g" \
    -e "s/@MDVWB_BUILT_AT@/$BUILT_AT/g" \
    "$SCRIPT_DIR/package-manifest.json.in" >"$STAGING/manifest.json"

(
    cd "$STAGING"
    find . -type f ! -name SHA256SUMS -print0 | sort -z |
        xargs -0 sha256sum >SHA256SUMS
    sha256sum -c SHA256SUMS
)

set -- "$OPERATION"
[ "$FORCE" -eq 0 ] || set -- "$@" --force
[ "$ALLOW_DOWNGRADE" -eq 0 ] || set -- "$@" --allow-downgrade
[ "$DRY_RUN" -eq 0 ] || set -- "$@" --dry-run
[ "$NO_BACKUP" -eq 0 ] || set -- "$@" --no-backup
[ -z "$BACKUP_DIR" ] || set -- "$@" --backup-dir "$BACKUP_DIR"

MDVWB_INSTALL_METHOD=source sh "$STAGING/offline-install.sh" "$@"
