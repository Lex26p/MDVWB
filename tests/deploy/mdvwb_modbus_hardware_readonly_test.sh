#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
CHECKER="$ROOT_DIR/tools/modbus-hardware-readonly-check.sh"

temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM
bin="$temporary/bin"
mkdir -p "$bin"
log="$temporary/calls.log"
state="$temporary/service-state"
printf 'active\n' >"$state"

cat >"$bin/systemctl" <<'MOCK'
#!/bin/sh
set -eu
printf 'systemctl %s\n' "$*" >>"$MOCK_LOG"
case "$1" in
    is-active)
        if [ "${2:-}" = "--quiet" ]; then
            service=${3:-}
        else
            service=${2:-}
        fi
        if [ "$service" = "mdvwb-manager.service" ]; then
            [ "${2:-}" = "--quiet" ] || printf 'active\n'
            exit 0
        fi
        if [ "$(cat "$MOCK_STATE")" = "active" ]; then
            [ "${2:-}" = "--quiet" ] || printf 'active\n'
            exit 0
        fi
        [ "${2:-}" = "--quiet" ] || printf 'inactive\n'
        exit 3
        ;;
    start)
        printf 'active\n' >"$MOCK_STATE"
        ;;
    stop)
        printf 'inactive\n' >"$MOCK_STATE"
        ;;
    status)
        printf 'mock status for %s\n' "${2:-}"
        ;;
    *) exit 2 ;;
esac
MOCK

cat >"$bin/mosquitto_pub" <<'MOCK'
#!/bin/sh
set -eu
printf 'mosquitto_pub %s\n' "$*" >>"$MOCK_LOG"
exit 0
MOCK

cat >"$bin/mosquitto_sub" <<'MOCK'
#!/bin/sh
set -eu
printf 'mosquitto_sub %s\n' "$*" >>"$MOCK_LOG"
case "$*" in
    *'/mdvwb/buses/7/discovery/status'*)
        printf '%s\n' \
            '/mdvwb/buses/7/discovery/status {"bus":7,"state":"idle"}' \
            '/mdvwb/buses/7/discovery/result {"success":true,"bus":7,"addresses":[63],"message":"stale"}' \
            '/mdvwb/buses/7/discovery/status {"bus":7,"state":"running"}' \
            '/mdvwb/buses/7/discovery/result {"success":true,"bus":7,"addresses":[1,3],"message":"Discovery completed"}' \
            '/mdvwb/buses/7/discovery/status {"bus":7,"state":"completed","found":2}'
        ;;
    *)
        printf '%s\n' \
            '/devices/Fan-7_1/controls/Power 1' \
            '/devices/Fan-7_1/controls/AlarmCode 0' \
            '/devices/Fan-7_3/controls/Power 0' \
            '/devices/Fan-7_3/controls/AlarmCode 0' \
            '/devices/sist-7/controls/Serial Modbus RTU порт открыт'
        ;;
esac
MOCK

cat >"$bin/journalctl" <<'MOCK'
#!/bin/sh
set -eu
printf 'journalctl %s\n' "$*" >>"$MOCK_LOG"
printf 'mock Modbus runtime journal\n'
MOCK

cat >"$bin/timeout" <<'MOCK'
#!/bin/sh
set -eu
shift
exec "$@"
MOCK

chmod 0755 "$bin/systemctl" "$bin/mosquitto_pub" "$bin/mosquitto_sub" \
    "$bin/journalctl" "$bin/timeout"

config="$temporary/mdvwb-7"
cat >"$config" <<'CONFIG'
MDVWB_ADDRESSES="1,3"
MDVWB_PORT="/dev/ttyRS485-1"
MDVWB_BUS="7"
MDVWB_PROTOCOL="modbus_rtu"
MDVWB_MODBUS_PROFILE="vrf_add_controller"
MDVWB_MQTT_HOST="127.0.0.1"
MDVWB_MQTT_PORT="1883"
MDVWB_MQTT_USER="operator"
MDVWB_MQTT_PASSWORD="do-not-record"
CONFIG

output="$temporary/evidence"
PATH="$bin:$PATH" MOCK_LOG="$log" MOCK_STATE="$state" \
    sh "$CHECKER" --bus 7 --config "$config" --output "$output" \
    --discovery-timeout 5 --capture-seconds 1 >"$temporary/stdout.txt"

grep -Fq 'MDVWB Modbus read-only hardware check: OK' "$temporary/stdout.txt"
grep -Fq 'discoveredAddresses=1,3' "$output/SUMMARY.txt"
grep -Fq 'addressesWithPowerAndAlarmCode=2' "$output/SUMMARY.txt"
grep -Fq 'powerCommandsPublished=false' "$output/manifest.txt"
if grep -Fq 'do-not-record' "$output/manifest.txt"; then
    printf 'MQTT password leaked into evidence manifest\n' >&2
    exit 1
fi

grep -Fq 'mosquitto_pub -h 127.0.0.1 -p 1883 -t /mdvwb/buses/7/discovery/start -m ' "$log"
grep -Fq -- '-t /devices/Fan-7_1/#' "$log"
grep -Fq -- '-t /devices/Fan-7_3/#' "$log"
if grep -Eq '/controls/.+/on1|/devices/.+/controls/(Power|Mode|Speed|SetTemp)/on' "$log"; then
    printf 'read-only checker published a device-control command\n' >&2
    exit 1
fi

# The stale retained result (address 63) arrived before the fresh running state
# and therefore must not be selected as this run's evidence.
[ "$(cat "$output/discovered-addresses.txt")" = '1,3' ]
[ "$(cat "$state")" = 'active' ]

mdv_config="$temporary/mdvwb-mdv"
sed 's/MDVWB_PROTOCOL="modbus_rtu"/MDVWB_PROTOCOL="mdv"/' "$config" >"$mdv_config"
: >"$log"
set +e
PATH="$bin:$PATH" MOCK_LOG="$log" MOCK_STATE="$state" \
    sh "$CHECKER" --bus 7 --config "$mdv_config" \
    --output "$temporary/mdv-evidence" >/dev/null 2>&1
status=$?
set -e
[ "$status" -ne 0 ] || {
    printf 'MDV bus was accepted by Modbus hardware checker\n' >&2
    exit 1
}
[ ! -s "$log" ] || {
    printf 'checker touched services or MQTT before rejecting MDV config\n' >&2
    exit 1
}

printf 'MDVWB Modbus hardware read-only checker tests: OK\n'
