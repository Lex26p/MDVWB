#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SETUP="$ROOT_DIR/deploy/mdvwb-setup"
OFFLINE="$ROOT_DIR/deploy/offline-install.sh"
TEMPORARY=${TMPDIR:-/tmp}/mdvwb-uninstall-test.$$
trap 'rm -rf "$TEMPORARY"' EXIT HUP INT TERM
mkdir -p "$TEMPORARY"

fail()
{
    printf 'mdvwb uninstall test failed: %s\n' "$*" >&2
    exit 1
}

expect_code()
{
    expected=$1
    shift
    set +e
    "$@" >"$TEMPORARY/stdout" 2>"$TEMPORARY/stderr"
    actual=$?
    set -e
    [ "$actual" -eq "$expected" ] ||
        fail "expected exit code $expected, got $actual: $*"
}

FAKEBIN="$TEMPORARY/fakebin"
ROOTFS="$TEMPORARY/root"
SYSTEMCTL_LOG="$TEMPORARY/systemctl.log"
MQTT_LOG="$TEMPORARY/mqtt.log"
mkdir -p "$FAKEBIN" "$ROOTFS"

cat >"$FAKEBIN/systemctl" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >>"$SYSTEMCTL_LOG"
case "\${1:-}" in
    is-active|is-enabled)
        exit 0
        ;;
esac
exit 0
EOF
chmod 0755 "$FAKEBIN/systemctl"

cat >"$FAKEBIN/mqtt-delete-retained" <<EOF
#!/bin/sh
printf '%s\n' "\$1" >>"$MQTT_LOG"
exit 0
EOF
chmod 0755 "$FAKEBIN/mqtt-delete-retained"

export PATH="$FAKEBIN:$PATH"
export MDVWB_ROOT="$ROOTFS"
export MDVWB_SYSTEMCTL="$FAKEBIN/systemctl"
export MDVWB_SERVICE_START_DELAY=0

populate_root()
{
    rm -rf "$ROOTFS"
    mkdir -p \
        "$ROOTFS/usr/local/bin" \
        "$ROOTFS/usr/local/sbin" \
        "$ROOTFS/usr/local/lib/mdvwb" \
        "$ROOTFS/etc/mdvwb" \
        "$ROOTFS/etc/default" \
        "$ROOTFS/etc/systemd/system" \
        "$ROOTFS/var/lib/mdvwb" \
        "$ROOTFS/var/log/mdvwb" \
        "$ROOTFS/var/www/mdvwb" \
        "$ROOTFS/var/www/fancoils/assets"

    for binary in MDVWB mdvwb-offline mdvwb-scheduler; do
        cat >"$ROOTFS/usr/local/bin/$binary" <<'EOF'
#!/bin/sh
exit 0
EOF
        chmod 0755 "$ROOTFS/usr/local/bin/$binary"
    done

    cat >"$ROOTFS/usr/local/bin/mdvwb-manager" <<'EOF'
#!/bin/sh
command=${1:-}
case "$command" in
    validate)
        exit 0
        ;;
    summary)
        echo "version=1"
        echo "buses=2"
        echo "enabled=1"
        echo "bus=1 enabled=true port=/dev/ttyRS485-1 addresses=1,7"
        echo "bus=2 enabled=false port=/dev/ttyRS485-2 addresses=3"
        ;;
    apply)
        exit 0
        ;;
    *)
        exit 2
        ;;
esac
EOF
    chmod 0755 "$ROOTFS/usr/local/bin/mdvwb-manager"

    cp "$SETUP" "$ROOTFS/usr/local/sbin/mdvwb-setup"
    chmod 0755 "$ROOTFS/usr/local/sbin/mdvwb-setup"
    printf 'runtime\n' >"$ROOTFS/usr/local/lib/mdvwb/mdvwb.env"

    for unit in \
        mdvwb@.service mdvwb-manager.service mdvwb-scheduler.service; do
        printf '[Unit]\nDescription=%s\n' "$unit" \
            >"$ROOTFS/etc/systemd/system/$unit"
    done

    printf 'manager-env\n' >"$ROOTFS/etc/default/mdvwb-manager"
    printf 'scheduler-env\n' >"$ROOTFS/etc/default/mdvwb-scheduler"
    printf 'bus-env\n' >"$ROOTFS/etc/default/mdvwb-1"
    printf 'disabled-bus-env\n' >"$ROOTFS/etc/default/mdvwb-2"

    cat >"$ROOTFS/etc/mdvwb/buses.json" <<'EOF'
{
  "version": 1,
  "buses": [
    {"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1,7]},
    {"id":2,"enabled":false,"port":"/dev/ttyRS485-2","addresses":[3]}
  ]
}
EOF
    printf 'dashboard-user\n' >"$ROOTFS/etc/mdvwb/dashboard.json"
    printf 'schedules-user\n' >"$ROOTFS/etc/mdvwb/schedules.json"

    cat >"$ROOTFS/var/lib/mdvwb/installation.json" <<'EOF'
{
  "stateFormat": 1,
  "product": "MDVWB",
  "version": "1.2.0",
  "commit": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "architecture": "arm64",
  "installedAt": "2026-07-29T10:00:00Z",
  "installMethod": "offline"
}
EOF
    printf 'scheduler-state\n' \
        >"$ROOTFS/var/lib/mdvwb/scheduler-state.tsv"
    printf 'old-log\n' >"$ROOTFS/var/log/mdvwb/existing.log"

    printf 'engineering-app\n' >"$ROOTFS/var/www/mdvwb/app.js"
    printf 'operator-app\n' >"$ROOTFS/var/www/fancoils/app.js"
    printf 'user-background\n' \
        >"$ROOTFS/var/www/fancoils/assets/user.png"
}

sh -n "$SETUP"
sh -n "$OFFLINE"
sh "$OFFLINE" --help >"$TEMPORARY/help"
grep -q 'uninstall --yes' "$TEMPORARY/help" ||
    fail "offline help does not mention uninstall"
grep -q 'purge --yes' "$TEMPORARY/help" ||
    fail "offline help does not mention purge"

populate_root
: >"$SYSTEMCTL_LOG"
: >"$MQTT_LOG"

sh "$SETUP" uninstall --dry-run >"$TEMPORARY/uninstall-plan"
grep -q '^ACTION=uninstall$' "$TEMPORARY/uninstall-plan" ||
    fail "uninstall dry-run action is missing"
grep -q '^preserve=/etc/mdvwb$' "$TEMPORARY/uninstall-plan" ||
    fail "uninstall plan does not preserve configuration"
grep -q '^DRY_RUN=complete$' "$TEMPORARY/uninstall-plan" ||
    fail "uninstall dry-run did not complete"
[ -x "$ROOTFS/usr/local/bin/MDVWB" ] ||
    fail "uninstall dry-run changed files"

expect_code 2 sh "$SETUP" uninstall
grep -q 'requires --yes' "$TEMPORARY/stderr" ||
    fail "uninstall without confirmation has no explanation"

sh "$SETUP" uninstall --yes >"$TEMPORARY/uninstall-output"
grep -q '^REMOVE_RESULT=success$' "$TEMPORARY/uninstall-output" ||
    fail "uninstall did not report success"
grep -q '^mode=uninstall$' "$TEMPORARY/uninstall-output" ||
    fail "uninstall mode is missing"

[ ! -e "$ROOTFS/usr/local/bin/MDVWB" ] ||
    fail "driver binary survived uninstall"
[ ! -e "$ROOTFS/usr/local/bin/mdvwb-manager" ] ||
    fail "manager binary survived uninstall"
[ ! -e "$ROOTFS/usr/local/sbin/mdvwb-setup" ] ||
    fail "lifecycle utility survived uninstall"
[ ! -e "$ROOTFS/etc/systemd/system/mdvwb-manager.service" ] ||
    fail "manager unit survived uninstall"
[ ! -e "$ROOTFS/var/www/mdvwb" ] ||
    fail "engineering web survived uninstall"
[ ! -e "$ROOTFS/var/www/fancoils/app.js" ] ||
    fail "operator application survived uninstall"

[ -s "$ROOTFS/etc/mdvwb/buses.json" ] ||
    fail "uninstall removed buses.json"
[ "$(cat "$ROOTFS/etc/mdvwb/dashboard.json")" = "dashboard-user" ] ||
    fail "uninstall changed dashboard"
[ "$(cat "$ROOTFS/etc/mdvwb/schedules.json")" = "schedules-user" ] ||
    fail "uninstall changed schedules"
[ -s "$ROOTFS/etc/default/mdvwb-1" ] ||
    fail "uninstall removed generated bus environment"
[ "$(cat "$ROOTFS/var/lib/mdvwb/scheduler-state.tsv")" = \
    "scheduler-state" ] || fail "uninstall removed scheduler state"
[ ! -e "$ROOTFS/var/lib/mdvwb/installation.json" ] ||
    fail "uninstall preserved installation state"
[ "$(cat "$ROOTFS/var/www/fancoils/assets/user.png")" = \
    "user-background" ] || fail "uninstall removed uploaded asset"
[ -s "$ROOTFS/var/log/mdvwb/existing.log" ] ||
    fail "uninstall removed logs"

find "$ROOTFS/var/backups/mdvwb" -name COMPLETE -type f |
    grep -q . || fail "uninstall did not create backup"

grep -q '^disable --now mdvwb-manager.service$' "$SYSTEMCTL_LOG" ||
    fail "uninstall did not disable manager"
grep -q '^disable --now mdvwb@1.service$' "$SYSTEMCTL_LOG" ||
    fail "uninstall did not disable enabled bus"
grep -q '^disable --now mdvwb@2.service$' "$SYSTEMCTL_LOG" ||
    fail "uninstall did not disable configured disabled bus"

for topic in \
    '/devices/Fan-1_1/#' \
    '/devices/Fan-1_7/#' \
    '/devices/sist-1/#' \
    '/devices/Fan-2_3/#' \
    '/devices/sist-2/#'; do
    grep -Fxq "$topic" "$MQTT_LOG" ||
        fail "owned retained topic was not removed: $topic"
done
if grep -Fxq '/devices/Fan-1_0/#' "$MQTT_LOG"; then
    fail "uninstall removed an unconfigured retained address"
fi

expect_code 3 sh "$SETUP" status
grep -q '^NOT_INSTALLED state=' "$TEMPORARY/stdout" ||
    fail "status after uninstall is incorrect"

populate_root
: >"$MQTT_LOG"

expect_code 2 sh "$SETUP" purge
grep -q 'requires --yes' "$TEMPORARY/stderr" ||
    fail "purge without confirmation has no explanation"

sh "$SETUP" purge --dry-run --remove-backups >"$TEMPORARY/purge-plan"
grep -q '^ACTION=purge$' "$TEMPORARY/purge-plan" ||
    fail "purge dry-run action is missing"
grep -q '^remove=/etc/mdvwb$' "$TEMPORARY/purge-plan" ||
    fail "purge plan does not remove configuration"
[ -s "$ROOTFS/etc/mdvwb/buses.json" ] ||
    fail "purge dry-run changed configuration"

sh "$SETUP" purge --yes --keep-retained --remove-backups \
    >"$TEMPORARY/purge-output"
grep -q '^REMOVE_RESULT=success$' "$TEMPORARY/purge-output" ||
    fail "purge did not report success"
grep -q '^mode=purge$' "$TEMPORARY/purge-output" ||
    fail "purge mode is missing"
grep -q '^backups=removed$' "$TEMPORARY/purge-output" ||
    fail "purge did not report backup removal"

[ ! -e "$ROOTFS/etc/mdvwb" ] ||
    fail "purge preserved configuration"
[ ! -e "$ROOTFS/etc/default/mdvwb-manager" ] ||
    fail "purge preserved manager environment"
[ ! -e "$ROOTFS/etc/default/mdvwb-1" ] ||
    fail "purge preserved bus environment"
[ ! -e "$ROOTFS/var/lib/mdvwb" ] ||
    fail "purge preserved application state"
[ ! -e "$ROOTFS/var/www/fancoils" ] ||
    fail "purge preserved operator web or assets"
[ ! -e "$ROOTFS/var/log/mdvwb" ] ||
    fail "purge preserved logs"
[ ! -d "$ROOTFS/var/backups/mdvwb" ] ||
    fail "purge --remove-backups preserved backup root"
[ ! -s "$MQTT_LOG" ] ||
    fail "--keep-retained still deleted MQTT topics"

populate_root
rm -f "$ROOTFS/var/lib/mdvwb/installation.json"
sh "$SETUP" uninstall --yes --force --no-backup --keep-retained \
    >"$TEMPORARY/force-output"
grep -q '^REMOVE_RESULT=success$' "$TEMPORARY/force-output" ||
    fail "forced cleanup of leftovers failed"

printf 'MDVWB uninstall and purge tests: OK\n'
