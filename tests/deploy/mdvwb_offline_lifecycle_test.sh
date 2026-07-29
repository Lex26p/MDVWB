#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SETUP="$ROOT_DIR/deploy/mdvwb-setup"
WRAPPER="$ROOT_DIR/deploy/offline-install.sh"
TEMPORARY=${TMPDIR:-/tmp}/mdvwb-offline-lifecycle-test.$$
trap 'rm -rf "$TEMPORARY"' EXIT HUP INT TERM
mkdir -p "$TEMPORARY"

fail()
{
    printf 'mdvwb offline lifecycle test failed: %s\n' "$*" >&2
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
    --version)
        echo "$program $version"
        ;;
    --self-test|--help)
        exit 0
        ;;
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
    cp "$WRAPPER" "$package/offline-install.sh"
    chmod 0755 "$package/mdvwb-setup" "$package/offline-install.sh"

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
        printf '%s\n' "$marker-$file" >"$package/www/mdvwb/$file"
    done

    for file in \
        index.html app.js model.js schedule-model.js scheduler-status-ui.js \
        scheduler-status-health.js styles.css; do
        printf '%s\n' "$marker-$file" >"$package/www/fancoils/$file"
    done

    (
        cd "$package"
        find . -type f ! -name SHA256SUMS -print0 | sort -z |
            xargs -0 sha256sum >SHA256SUMS
    )
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
        service=\${3:-\${2:-}}
        if [ -n "\${MDVWB_FAIL_SERVICE:-}" ] &&
           [ "\$service" = "\$MDVWB_FAIL_SERVICE" ]; then
            exit 1
        fi
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

make_package "$PACKAGE" 1.2.0 first

sh "$PACKAGE/offline-install.sh" --help >"$TEMPORARY/help"
grep -q 'offline-install.sh verify' "$TEMPORARY/help" ||
    fail "offline help does not document verify"
grep -q 'offline-install.sh status' "$TEMPORARY/help" ||
    fail "offline help does not document status"

sh "$PACKAGE/offline-install.sh" version >"$TEMPORARY/version"
grep -q '^PACKAGE_OK product=MDVWB version=1.2.0$' "$TEMPORARY/version" ||
    fail "offline package version is incorrect"

sh "$PACKAGE/offline-install.sh" verify --package-only >"$TEMPORARY/package-verify"
grep -q '^VERIFY_RESULT=success$' "$TEMPORARY/package-verify" ||
    fail "package-only verification failed"
grep -q '^scope=package$' "$TEMPORARY/package-verify" ||
    fail "package-only verification scope is incorrect"

sh "$PACKAGE/offline-install.sh" --verify-only >"$TEMPORARY/controller-verify"
grep -q '^scope=controller$' "$TEMPORARY/controller-verify" ||
    fail "controller verification scope is incorrect"

cp "$PACKAGE/www/fancoils/styles.css" "$TEMPORARY/styles.css"
rm "$PACKAGE/www/fancoils/styles.css"
expect_code 2 sh "$PACKAGE/offline-install.sh" verify --package-only
grep -q 'file does not exist' "$TEMPORARY/stderr" ||
    fail "incomplete package was not explained"
cp "$TEMPORARY/styles.css" "$PACKAGE/www/fancoils/styles.css"
(
    cd "$PACKAGE"
    find . -type f ! -name SHA256SUMS -print0 | sort -z |
        xargs -0 sha256sum >SHA256SUMS
)

printf '%s\n' "user-background" \
    >"$ROOTFS/var/www/fancoils/assets/user.png"

sh "$PACKAGE/offline-install.sh" --dry-run >"$TEMPORARY/default-dry-run"
grep -q '^ACTION=install current=none target=1.2.0 architecture=arm64$' \
    "$TEMPORARY/default-dry-run" ||
    fail "leading install option was not routed to install"
grep -q '^DRY_RUN=complete$' "$TEMPORARY/default-dry-run" ||
    fail "default dry-run did not complete"

sh "$PACKAGE/offline-install.sh" install >"$TEMPORARY/install-output"
grep -q '^MDVWB_RESULT=success$' "$TEMPORARY/install-output" ||
    fail "offline install did not report success"
grep -q '^action=install$' "$TEMPORARY/install-output" ||
    fail "offline install action is incorrect"
[ -x "$ROOTFS/usr/local/bin/MDVWB" ] ||
    fail "offline install did not copy MDVWB"
[ -x "$ROOTFS/usr/local/sbin/mdvwb-setup" ] ||
    fail "offline install did not copy mdvwb-setup"
[ "$(cat "$ROOTFS/var/www/fancoils/assets/user.png")" = "user-background" ] ||
    fail "offline install removed uploaded assets"

sh "$PACKAGE/offline-install.sh" status >"$TEMPORARY/status"
grep -q '^INSTALLED product=MDVWB version=1.2.0$' "$TEMPORARY/status" ||
    fail "offline status does not show installed version"
grep -q '^configuration=buses.json state=valid$' "$TEMPORARY/status" ||
    fail "offline status does not validate buses.json"
grep -q '^service=mdvwb-manager.service active=true enabled=true required=true$' \
    "$TEMPORARY/status" || fail "manager health is missing"
grep -q '^service=mdvwb@1.service active=true enabled=true required=true$' \
    "$TEMPORARY/status" || fail "enabled bus health is missing"
grep -q '^HEALTH=OK$' "$TEMPORARY/status" ||
    fail "healthy installation was not reported"

install_log=$(find "$ROOTFS/var/log/mdvwb" -type f -name '*-install-*.log' |
    sort | tail -n 1)
[ -n "$install_log" ] || fail "install operation log was not created"
grep -q '^operation=install$' "$install_log" ||
    fail "install operation log has no operation"
grep -q '^finalStatus=success$' "$install_log" ||
    fail "install operation log has no success result"

backup_output=$(sh "$PACKAGE/offline-install.sh" backup)
printf '%s\n' "$backup_output" | grep -q '^BACKUP_CREATED path=' ||
    fail "offline backup command was not routed"

make_package "$PACKAGE" 1.3.0 second
sh "$PACKAGE/offline-install.sh" update >"$TEMPORARY/update-output"
grep -q '^action=update$' "$TEMPORARY/update-output" ||
    fail "offline update action is incorrect"
grep -q '"version": "1.3.0"' "$ROOTFS/var/lib/mdvwb/installation.json" ||
    fail "offline update did not update installation state"
[ "$(cat "$ROOTFS/var/www/fancoils/app.js")" = "second-app.js" ] ||
    fail "offline update did not replace operator application"

update_log=$(find "$ROOTFS/var/log/mdvwb" -type f -name '*-update-*.log' |
    sort | tail -n 1)
[ -n "$update_log" ] || fail "update operation log was not created"
grep -q '^finalStatus=success$' "$update_log" ||
    fail "update operation log has no success result"

MDVWB_FAIL_SERVICE=mdvwb-scheduler.service
export MDVWB_FAIL_SERVICE
expect_code 7 sh "$PACKAGE/offline-install.sh" status
grep -q '^HEALTH=FAILED$' "$TEMPORARY/stdout" ||
    fail "failed service health was not reported"
grep -q 'service=mdvwb-scheduler.service active=false' "$TEMPORARY/stdout" ||
    fail "failed scheduler state is missing"
unset MDVWB_FAIL_SERVICE

cp "$ROOTFS/etc/mdvwb/buses.json" "$TEMPORARY/buses.json"
cat >"$ROOTFS/etc/mdvwb/buses.json" <<'EOF'
{
  "version": 1,
  "invalid": true,
  "buses": []
}
EOF
expect_code 7 sh "$PACKAGE/offline-install.sh" status
grep -q '^configuration=buses.json state=invalid$' "$TEMPORARY/stdout" ||
    fail "invalid installed configuration was not reported"
grep -q '^HEALTH=FAILED$' "$TEMPORARY/stdout" ||
    fail "invalid configuration did not fail health"
cp "$TEMPORARY/buses.json" "$ROOTFS/etc/mdvwb/buses.json"

printf '%s\n' "corruption" >>"$PACKAGE/www/fancoils/app.js"
expect_code 1 sh "$PACKAGE/offline-install.sh" verify --package-only
grep -Eq 'FAILED|did NOT match|checksum' "$TEMPORARY/stderr" ||
    fail "checksum corruption was not reported"

expect_code 2 sh "$PACKAGE/offline-install.sh" unknown
grep -q 'Unknown offline command' "$TEMPORARY/stderr" ||
    fail "unknown offline command has no explanation"

printf 'MDVWB offline install and update tests: OK\n'
