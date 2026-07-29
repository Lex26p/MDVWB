#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SETUP="$ROOT_DIR/deploy/mdvwb-setup"
TEMPORARY=${TMPDIR:-/tmp}/mdvwb-install-engine-test.$$
trap 'rm -rf "$TEMPORARY"' EXIT HUP INT TERM
mkdir -p "$TEMPORARY"

fail()
{
    printf 'mdvwb install engine test failed: %s\n' "$*" >&2
    exit 1
}

write_executable()
{
    path=$1
    shift
    cat >"$path"
    chmod 0755 "$path"
}

make_manifest()
{
    path=$1
    version=$2
    cat >"$path" <<EOF
{
  "packageFormat": 1,
  "product": "MDVWB",
  "version": "$version",
  "commit": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "architecture": "arm64",
  "buildOs": "debian-bullseye",
  "builtAt": "2026-07-29T10:00:00Z"
}
EOF
}

make_package()
{
    package=$1
    version=$2
    marker=$3
    rm -rf "$package"
    mkdir -p "$package/www/mdvwb" "$package/www/fancoils"

    for program in MDVWB mdvwb-offline mdvwb-scheduler; do
        cat >"$package/$program" <<EOF
#!/bin/sh
case "\${1:-}" in
    --version) echo "$program $version" ;;
    --self-test|--help) exit 0 ;;
esac
exit 0
EOF
        chmod 0755 "$package/$program"
    done

    cat >"$package/mdvwb-manager" <<'EOF'
#!/bin/sh
command=${1:-}
config=${2:-}
case "$command" in
    validate)
        [ -s "$config" ] || exit 2
        grep -q '"invalid"[[:space:]]*:[[:space:]]*true' "$config" && exit 2
        echo "CONFIG_OK buses=1 enabled=1"
        ;;
    summary)
        echo "version=1"
        echo "buses=1"
        echo "enabled=1"
        echo "bus=1 enabled=true port=/dev/ttyRS485-1 addresses=1"
        ;;
    migrate-defaults)
        exit 3
        ;;
    apply)
        echo "APPLIED actions=1"
        ;;
    *)
        exit 2
        ;;
esac
EOF
    chmod 0755 "$package/mdvwb-manager"
    cp "$SETUP" "$package/mdvwb-setup"
    chmod 0755 "$package/mdvwb-setup"

    cat >"$package/mdvwb-run" <<'EOF'
#!/bin/sh
exit 0
EOF
    chmod 0755 "$package/mdvwb-run"

    for file in mdvwb@.service mdvwb-manager.service mdvwb-scheduler.service; do
        printf '[Unit]\nDescription=%s\n' "$file" >"$package/$file"
    done
    for file in mdvwb.env mdvwb-manager.env mdvwb-scheduler.env; do
        printf 'MDVWB_TEST=1\n' >"$package/$file"
    done

    cat >"$package/buses.default.json" <<'EOF'
{
  "version": 1,
  "buses": []
}
EOF
    printf '{"version":1,"revision":0,"panels":[]}\n' \
        >"$package/dashboard.default.json"
    printf '{"version":1,"revision":0,"schedules":[]}\n' \
        >"$package/schedules.default.json"

    make_manifest "$package/manifest.json" "$version"
    printf '%s\n' "$marker" >"$package/www/mdvwb/index.html"
    printf '%s\n' "$marker" >"$package/www/mdvwb/app.js"
    printf '%s\n' "$marker" >"$package/www/mdvwb/model.js"
    printf '%s\n' "$marker" >"$package/www/mdvwb/mqtt-client.js"
    printf '%s\n' "$marker" >"$package/www/mdvwb/styles.css"
    printf '%s\n' "$marker" >"$package/www/mdvwb/dashboard-editor.js"
    printf '%s\n' "$marker" >"$package/www/mdvwb/dashboard-model.js"
    printf '%s\n' "$marker" >"$package/www/mdvwb/dashboard-placement-editor.js"
    printf '%s\n' "$marker" >"$package/www/fancoils/index.html"
    printf '%s\n' "$marker" >"$package/www/fancoils/app.js"
    printf '%s\n' "$marker" >"$package/www/fancoils/model.js"
    printf '%s\n' "$marker" >"$package/www/fancoils/schedule-model.js"
    printf '%s\n' "$marker" >"$package/www/fancoils/scheduler-status-ui.js"
    printf '%s\n' "$marker" >"$package/www/fancoils/scheduler-status-health.js"
    printf '%s\n' "$marker" >"$package/www/fancoils/styles.css"

    (
        cd "$package"
        find . -type f ! -name SHA256SUMS -print0 | sort -z |
            xargs -0 sha256sum >SHA256SUMS
    )
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
PACKAGE="$TEMPORARY/package"
SYSTEMCTL_LOG="$TEMPORARY/systemctl.log"
mkdir -p "$FAKEBIN" "$ROOTFS/etc/wb-rules" "$ROOTFS/var/www/fancoils/assets"

cat >"$FAKEBIN/dpkg" <<'EOF'
#!/bin/sh
[ "${1:-}" = "--print-architecture" ] && echo arm64
EOF
chmod 0755 "$FAKEBIN/dpkg"

cat >"$FAKEBIN/ldconfig" <<'EOF'
#!/bin/sh
echo "libmosquitto.so.1"
EOF
chmod 0755 "$FAKEBIN/ldconfig"

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

export MDVWB_ROOT="$ROOTFS"
export MDVWB_DPKG="$FAKEBIN/dpkg"
export MDVWB_LDCONFIG="$FAKEBIN/ldconfig"
export MDVWB_SYSTEMCTL="$FAKEBIN/systemctl"
export MDVWB_SERVICE_START_DELAY=0

printf '%s\n' "user-background" \
    >"$ROOTFS/var/www/fancoils/assets/user.png"

make_package "$PACKAGE" 1.2.0 first
sh "$SETUP" install --package "$PACKAGE" --method offline \
    >"$TEMPORARY/install-output"

grep -q '^MDVWB_RESULT=success$' "$TEMPORARY/install-output" ||
    fail "fresh installation did not report success"
grep -q '^action=install$' "$TEMPORARY/install-output" ||
    fail "fresh installation action is wrong"
[ -x "$ROOTFS/usr/local/bin/MDVWB" ] ||
    fail "driver binary was not installed"
[ -x "$ROOTFS/usr/local/sbin/mdvwb-setup" ] ||
    fail "setup utility was not installed"
[ -s "$ROOTFS/etc/mdvwb/buses.json" ] ||
    fail "buses.json was not created"
grep -q '"buses":[[:space:]]*\[\]' "$ROOTFS/etc/mdvwb/buses.json" ||
    fail "fresh buses.json is not empty"
[ "$(cat "$ROOTFS/var/www/fancoils/assets/user.png")" = "user-background" ] ||
    fail "uploaded background was removed"
grep -q '"version": "1.2.0"' "$ROOTFS/var/lib/mdvwb/installation.json" ||
    fail "installation state has the wrong fresh version"
grep -q '^enable --now mdvwb-manager.service$' "$SYSTEMCTL_LOG" ||
    fail "manager service was not enabled"
grep -q '^is-active --quiet mdvwb@1.service$' "$SYSTEMCTL_LOG" ||
    fail "enabled bus service was not verified"

cat >"$ROOTFS/etc/mdvwb/buses.json" <<'EOF'
{
  "version": 1,
  "buses": [
    {"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1]}
  ]
}
EOF
printf '%s\n' "user-dashboard" >"$ROOTFS/etc/mdvwb/dashboard.json"
printf '%s\n' "user-schedules" >"$ROOTFS/etc/mdvwb/schedules.json"
printf '%s\n' "user-background-updated" \
    >"$ROOTFS/var/www/fancoils/assets/user.png"

make_package "$PACKAGE" 1.3.0 second
sh "$SETUP" update --package "$PACKAGE" --method offline \
    >"$TEMPORARY/update-output"

grep -q '^action=update$' "$TEMPORARY/update-output" ||
    fail "update action is wrong"
grep -q 'ttyRS485-1' "$ROOTFS/etc/mdvwb/buses.json" ||
    fail "existing buses.json was replaced"
[ "$(cat "$ROOTFS/etc/mdvwb/dashboard.json")" = "user-dashboard" ] ||
    fail "dashboard.json was replaced"
[ "$(cat "$ROOTFS/etc/mdvwb/schedules.json")" = "user-schedules" ] ||
    fail "schedules.json was replaced"
[ "$(cat "$ROOTFS/var/www/fancoils/assets/user.png")" = \
    "user-background-updated" ] || fail "uploaded background was replaced"
[ "$(cat "$ROOTFS/var/www/fancoils/app.js")" = "second" ] ||
    fail "operator application was not updated"
grep -q '"version": "1.3.0"' "$ROOTFS/var/lib/mdvwb/installation.json" ||
    fail "installation state was not updated"

before_log_lines=$(wc -l <"$SYSTEMCTL_LOG")
expect_code 4 sh "$SETUP" update \
    --package "$PACKAGE" --method offline
after_log_lines=$(wc -l <"$SYSTEMCTL_LOG")
[ "$before_log_lines" -eq "$after_log_lines" ] ||
    fail "same-version rejection changed services"

sh "$SETUP" update --package "$PACKAGE" --method offline --force --dry-run \
    >"$TEMPORARY/dry-run-output"
grep -q '^ACTION=repair current=1.3.0 target=1.3.0 architecture=arm64$' \
    "$TEMPORARY/dry-run-output" || fail "repair dry-run plan is wrong"
grep -q '^DRY_RUN=complete$' "$TEMPORARY/dry-run-output" ||
    fail "dry-run did not complete"

printf '%s\n' "corruption" >>"$PACKAGE/www/fancoils/app.js"
before_log_lines=$(wc -l <"$SYSTEMCTL_LOG")
expect_code 1 sh "$SETUP" update \
    --package "$PACKAGE" --method offline --force
after_log_lines=$(wc -l <"$SYSTEMCTL_LOG")
[ "$before_log_lines" -eq "$after_log_lines" ] ||
    fail "checksum failure changed services"
grep -Eq 'FAILED|did NOT match|checksum' "$TEMPORARY/stderr" ||
    fail "checksum failure was not reported"

printf 'MDVWB common installation engine tests: OK\n'
