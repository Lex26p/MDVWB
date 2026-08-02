#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
WRAPPER="$ROOT_DIR/deploy/mdvwb-run"

temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT HUP INT TERM

runtime="$temporary/mdvwb-modbus"
cat >"$runtime" <<'RUNTIME_EOF'
#!/bin/sh
set -eu
printf '%s\n' \
    "command=$MDVWB_MODBUS_COMMAND_PERIOD_MS" \
    "retry=$MDVWB_MODBUS_RETRY_PERIOD_MS" \
    "writes=$MDVWB_MODBUS_WRITE_ATTEMPTS" \
    "confirmations=$MDVWB_MODBUS_CONFIRMATION_ATTEMPTS" \
    "burst=$MDVWB_MODBUS_PRIORITY_BURST"
RUNTIME_EOF
chmod 0755 "$runtime"

write_config()
{
    target=$1
    include_policy=$2
    cat >"$target" <<'CONFIG_EOF'
MDVWB_ADDRESSES="1"
MDVWB_PORT="/dev/ttyRS485-test"
MDVWB_BUS="7"
MDVWB_PROTOCOL="modbus_rtu"
MDVWB_MODBUS_PROFILE="vrf_add_controller"
MDVWB_MODBUS_PROFILE_DIR="/usr/local/lib/mdvwb/modbus-profiles"
MDVWB_MODBUS_BAUD_RATE="9600"
MDVWB_MODBUS_DATA_BITS="8"
MDVWB_MODBUS_PARITY="none"
MDVWB_MODBUS_STOP_BITS="1"
CONFIG_EOF
    if [ "$include_policy" = "1" ]; then
        cat >>"$target" <<'POLICY_EOF'
MDVWB_MODBUS_COMMAND_PERIOD_MS="31"
MDVWB_MODBUS_RETRY_PERIOD_MS="701"
MDVWB_MODBUS_WRITE_ATTEMPTS="5"
MDVWB_MODBUS_CONFIRMATION_ATTEMPTS="6"
MDVWB_MODBUS_PRIORITY_BURST="7"
POLICY_EOF
    fi
}

custom_config="$temporary/custom.env"
write_config "$custom_config" 1
custom_output=$(MDVWB_CONFIG_FILE="$custom_config" MDVWB_MODBUS_BINARY="$runtime" sh "$WRAPPER")
custom_expected='command=31
retry=701
writes=5
confirmations=6
burst=7'
[ "$custom_output" = "$custom_expected" ] || {
    printf 'custom Modbus runtime policy was not exported:\n%s\n' "$custom_output" >&2
    exit 1
}

default_config="$temporary/default.env"
write_config "$default_config" 0
default_output=$(MDVWB_CONFIG_FILE="$default_config" MDVWB_MODBUS_BINARY="$runtime" sh "$WRAPPER")
default_expected='command=20
retry=500
writes=3
confirmations=3
burst=4'
[ "$default_output" = "$default_expected" ] || {
    printf 'default Modbus runtime policy was not exported:\n%s\n' "$default_output" >&2
    exit 1
}

printf 'MDVWB Modbus runtime handoff tests: OK\n'
