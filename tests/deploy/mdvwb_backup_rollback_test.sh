#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SETUP="$ROOT_DIR/deploy/mdvwb-setup"
TEMPORARY=${TMPDIR:-/tmp}/mdvwb-backup-rollback-test.$$
trap 'rm -rf "$TEMPORARY"' EXIT HUP INT TERM
mkdir -p "$TEMPORARY"

fail()
{
    printf 'mdvwb backup/rollback test failed: %s\n' "$*" >&2
    exit 1
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

    cat >"$package/MDVWB" <<EOF
#!/bin/sh
case "\${1:-}" in
    --version) echo "MDVWB $version" ;;
    --self-test) exit 0 ;;
esac
printf '%s\n' "$marker"
exit 0
EOF
    chmod 0755 "$package/MDVWB"

    for program in mdvwb-offline mdvwb-scheduler; do
        cat >"$package/$program" <<EOF
#!/bin/sh
case "\${1:-}" in
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
        mkdir -p "$MDVWB_ROOT/etc/default"
        printf '%s\n' "generated-bus-environment-@VERSION@" \
            >"$MDVWB_ROOT/etc/default/mdvwb-1"
        "$MDVWB_SYSTEMCTL" enable --now mdvwb@1.service
        echo "APPLIED actions=1"
        ;;
    *)
        exit 2
        ;;
esac
EOF
    sed -i "s/@VERSION@/$version/g" "$package/mdvwb-manager"
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

    for file in \
        index.html app.js model.js mqtt-client.js styles.css \
        dashboard-editor.js dashboard-model.js dashboard-placement-editor.js; do
        printf '%s\n' "$marker" >"$package/www/mdvwb/$file"
    done
    for file in \
        index.html app.js model.js schedule-model.js \
        scheduler-status-ui.js scheduler-status-health.js styles.css; do
        printf '%s\n' "$marker" >"$package/www/fancoils/$file"
    done

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

STATE_DIR="$TEMPORARY/systemctl-state"
ACTIVE="$STATE_DIR/active"
ENABLED="$STATE_DIR/enabled"
SYSTEMCTL_LOG="$TEMPORARY/systemctl.log"
FAIL_ONCE="$STATE_DIR/fail-scheduler-once"
mkdir -p "$STATE_DIR"
: >"$ACTIVE"
: >"$ENABLED"
: >"$SYSTEMCTL_LOG"

FAKEBIN="$TEMPORARY/fakebin"
ROOTFS="$TEMPORARY/root"
PACKAGE="$TEMPORARY/package"
BACKUPS="$ROOTFS/var/backups/mdvwb"
MANUAL_BACKUPS="$TEMPORARY/manual-backups"
mkdir -p "$FAKEBIN" "$ROOTFS/etc/wb-rules" \
    "$ROOTFS/var/www/fancoils/assets"

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
set -eu
ACTIVE='$ACTIVE'
ENABLED='$ENABLED'
LOG='$SYSTEMCTL_LOG'
FAIL_ONCE='$FAIL_ONCE'

contains()
{
    file=\$1
    value=\$2
    grep -Fxq "\$value" "\$file" 2>/dev/null
}

add()
{
    file=\$1
    value=\$2
    contains "\$file" "\$value" || printf '%s\n' "\$value" >>"\$file"
}

remove()
{
    file=\$1
    value=\$2
    temporary="\$file.tmp.\$\$"
    grep -Fxv "\$value" "\$file" >"\$temporary" || true
    mv -f "\$temporary" "\$file"
}

printf '%s\n' "\$*" >>"\$LOG"
command=\${1:-}
shift || true

case "\$command" in
    is-active)
        [ "\${1:-}" = "--quiet" ] && shift
        contains "\$ACTIVE" "\${1:-}"
        ;;
    is-enabled)
        [ "\${1:-}" = "--quiet" ] && shift
        contains "\$ENABLED" "\${1:-}"
        ;;
    enable)
        now=0
        [ "\${1:-}" = "--now" ] && { now=1; shift; }
        for service in "\$@"; do
            if [ "\$service" = "mdvwb-scheduler.service" ] &&
               [ -f "\$FAIL_ONCE" ]; then
                rm -f "\$FAIL_ONCE"
                exit 1
            fi
            add "\$ENABLED" "\$service"
            [ "\$now" -eq 0 ] || add "\$ACTIVE" "\$service"
        done
        ;;
    disable)
        now=0
        [ "\${1:-}" = "--now" ] && { now=1; shift; }
        for service in "\$@"; do
            remove "\$ENABLED" "\$service"
            [ "\$now" -eq 0 ] || remove "\$ACTIVE" "\$service"
        done
        ;;
    start|restart)
        for service in "\$@"; do
            add "\$ACTIVE" "\$service"
        done
        ;;
    stop)
        for service in "\$@"; do
            remove "\$ACTIVE" "\$service"
        done
        ;;
    daemon-reload|status)
        ;;
    *)
        exit 2
        ;;
esac
EOF
chmod 0755 "$FAKEBIN/systemctl"

export MDVWB_ROOT="$ROOTFS"
export MDVWB_DPKG="$FAKEBIN/dpkg"
export MDVWB_LDCONFIG="$FAKEBIN/ldconfig"
export MDVWB_SYSTEMCTL="$FAKEBIN/systemctl"
export MDVWB_SERVICE_START_DELAY=0

printf '%s\n' "user-background" \
    >"$ROOTFS/var/www/fancoils/assets/user.png"

make_package "$PACKAGE" 1.2.0 v120
sh "$SETUP" install --package "$PACKAGE" --method offline \
    >"$TEMPORARY/install-output"
grep -q '^BACKUP_CREATED path=' "$TEMPORARY/install-output" ||
    fail "fresh installation did not create an automatic backup"

cat >"$ROOTFS/etc/mdvwb/buses.json" <<'EOF'
{
  "version": 1,
  "buses": [
    {"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1]}
  ]
}
EOF
printf '%s\n' "dashboard-v120" >"$ROOTFS/etc/mdvwb/dashboard.json"
printf '%s\n' "schedules-v120" >"$ROOTFS/etc/mdvwb/schedules.json"
printf '%s\n' "state-v120" >"$ROOTFS/var/lib/mdvwb/scheduler-state.tsv"

make_package "$PACKAGE" 1.3.0 v130
sh "$SETUP" update --package "$PACKAGE" --method offline \
    >"$TEMPORARY/update-output"

[ "$(cat "$ROOTFS/var/www/fancoils/app.js")" = "v130" ] ||
    fail "successful update did not install version 1.3.0 web files"
grep -q '"version": "1.3.0"' "$ROOTFS/var/lib/mdvwb/installation.json" ||
    fail "successful update did not write version 1.3.0 state"

sh "$SETUP" backup --backup-dir "$MANUAL_BACKUPS" \
    >"$TEMPORARY/manual-backup-output"
MANUAL_BACKUP=$(sed -n 's/^BACKUP_CREATED path=//p' \
    "$TEMPORARY/manual-backup-output")
[ -d "$MANUAL_BACKUP" ] || fail "manual backup directory was not created"
[ -f "$MANUAL_BACKUP/COMPLETE" ] ||
    fail "manual backup was not marked complete"
grep -q '^MDVWB_BACKUP_VERSION=1.3.0$' "$MANUAL_BACKUP/backup.conf" ||
    fail "manual backup did not record the installed version"

printf '%s\n' "changed-dashboard" >"$ROOTFS/etc/mdvwb/dashboard.json"
printf '%s\n' "changed-web" >"$ROOTFS/var/www/fancoils/app.js"
printf '%s\n' "changed-background" \
    >"$ROOTFS/var/www/fancoils/assets/user.png"

sh "$SETUP" rollback --backup "$MANUAL_BACKUP" \
    >"$TEMPORARY/manual-rollback-output"
grep -q '^ROLLBACK_RESULT=success$' "$TEMPORARY/manual-rollback-output" ||
    fail "manual rollback did not report success"
[ "$(cat "$ROOTFS/etc/mdvwb/dashboard.json")" = "dashboard-v120" ] ||
    fail "manual rollback did not restore dashboard"
[ "$(cat "$ROOTFS/var/www/fancoils/app.js")" = "v130" ] ||
    fail "manual rollback did not restore web application"
[ "$(cat "$ROOTFS/var/www/fancoils/assets/user.png")" = "user-background" ] ||
    fail "manual rollback did not restore uploaded background"

rm -f "$ROOTFS/etc/default/mdvwb-1"
make_package "$PACKAGE" 1.4.0 v140
touch "$FAIL_ONCE"
expect_code 1 sh "$SETUP" update \
    --package "$PACKAGE" --method offline

grep -q '^AUTOMATIC_ROLLBACK=completed$' "$TEMPORARY/stderr" ||
    fail "failed update did not complete automatic rollback"
grep -q '^ROLLBACK_RESULT=success$' "$TEMPORARY/stdout" ||
    fail "automatic rollback did not report restoration"

[ "$(cat "$ROOTFS/var/www/fancoils/app.js")" = "v130" ] ||
    fail "automatic rollback did not restore previous web version"
grep -q '"version": "1.3.0"' "$ROOTFS/var/lib/mdvwb/installation.json" ||
    fail "automatic rollback did not restore installation state"
[ "$(cat "$ROOTFS/etc/default/mdvwb-1")" =     "generated-bus-environment-1.3.0" ] ||
    fail "automatic rollback did not restore the previous bus environment"
[ "$(cat "$ROOTFS/etc/mdvwb/dashboard.json")" = "dashboard-v120" ] ||
    fail "automatic rollback did not restore dashboard"
[ "$(cat "$ROOTFS/etc/mdvwb/schedules.json")" = "schedules-v120" ] ||
    fail "automatic rollback did not restore schedules"
[ "$(cat "$ROOTFS/var/lib/mdvwb/scheduler-state.tsv")" = "state-v120" ] ||
    fail "automatic rollback did not restore scheduler state"
[ "$(cat "$ROOTFS/var/www/fancoils/assets/user.png")" = "user-background" ] ||
    fail "automatic rollback did not restore uploaded background"

grep -Fxq 'mdvwb-manager.service' "$ACTIVE" ||
    fail "automatic rollback did not restore manager active state"
grep -Fxq 'mdvwb-scheduler.service' "$ACTIVE" ||
    fail "automatic rollback did not restore scheduler active state"
grep -Fxq 'mdvwb@1.service' "$ACTIVE" ||
    fail "automatic rollback did not restore bus active state"
grep -Fxq 'mdvwb-manager.service' "$ENABLED" ||
    fail "automatic rollback did not restore manager enabled state"
grep -Fxq 'mdvwb-scheduler.service' "$ENABLED" ||
    fail "automatic rollback did not restore scheduler enabled state"
grep -Fxq 'mdvwb@1.service' "$ENABLED" ||
    fail "automatic rollback did not restore bus enabled state"

make_package "$PACKAGE" 1.4.0 v140
touch "$FAIL_ONCE"
expect_code 1 sh "$SETUP" update \
    --package "$PACKAGE" --method offline --no-backup
grep -q '^ROLLBACK_RESULT=skipped reason=no-backup$' "$TEMPORARY/stderr" ||
    fail "--no-backup did not explicitly skip rollback"
[ "$(cat "$ROOTFS/var/www/fancoils/app.js")" = "v140" ] ||
    fail "--no-backup unexpectedly restored the previous web version"

printf 'MDVWB backup and rollback tests: OK\n'
