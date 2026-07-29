#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
ONLINE="$ROOT_DIR/deploy/online-install.sh"
COMPAT="$ROOT_DIR/deploy/install_wirenboard.sh"
TEMPORARY=${TMPDIR:-/tmp}/mdvwb-online-test.$$
trap 'rm -rf "$TEMPORARY"' EXIT HUP INT TERM
mkdir -p "$TEMPORARY"

fail()
{
    printf 'mdvwb online installer test failed: %s\n' "$*" >&2
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

    cp "$ROOT_DIR/deploy/mdvwb-setup" "$package/mdvwb-setup"
    cp "$ROOT_DIR/deploy/offline-install.sh" "$package/offline-install.sh"
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
        dashboard-editor.js dashboard-model.js \
        dashboard-placement-editor.js; do
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

make_release()
{
    release=$1
    version=$2
    marker=$3
    release_package_root="$TEMPORARY/package-$version"
    rm -rf "$release" "$release_package_root"
    mkdir -p "$release"
    make_package "$release_package_root/MDVWB-arm64" "$version" "$marker"
    tar -C "$release_package_root" -czf "$release/MDVWB-arm64-offline.tar.gz" \
        MDVWB-arm64
    (
        cd "$release"
        sha256sum MDVWB-arm64-offline.tar.gz |
            sed 's#  #  dist/#' \
            >MDVWB-arm64-offline.tar.gz.sha256
    )
}

FAKEBIN="$TEMPORARY/fakebin"
ROOTFS="$TEMPORARY/root"
LATEST_RELEASE="$TEMPORARY/releases/latest"
V130_RELEASE="$TEMPORARY/releases/v1.3.0"
BAD_RELEASE="$TEMPORARY/releases/bad"
WRONG_ROOT_RELEASE="$TEMPORARY/releases/wrong-root"
CURL_LOG="$TEMPORARY/curl.log"
SYSTEMCTL_LOG="$TEMPORARY/systemctl.log"
mkdir -p \
    "$FAKEBIN" \
    "$ROOTFS/etc/wb-rules" \
    "$ROOTFS/var/www/fancoils/assets" \
    "$TEMPORARY/downloads"

make_release "$LATEST_RELEASE" 1.2.0 first
make_release "$V130_RELEASE" 1.3.0 second
cp -a "$LATEST_RELEASE" "$BAD_RELEASE"
printf '%064d  MDVWB-arm64-offline.tar.gz\n' 0 \
    >"$BAD_RELEASE/MDVWB-arm64-offline.tar.gz.sha256"

mkdir -p "$TEMPORARY/wrong-root/wrong"
printf 'unsafe\n' >"$TEMPORARY/wrong-root/wrong/file"
mkdir -p "$WRONG_ROOT_RELEASE"
tar -C "$TEMPORARY/wrong-root" -czf \
    "$WRONG_ROOT_RELEASE/MDVWB-arm64-offline.tar.gz" wrong
(
    cd "$WRONG_ROOT_RELEASE"
    sha256sum MDVWB-arm64-offline.tar.gz \
        >MDVWB-arm64-offline.tar.gz.sha256
)

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

cat >"$FAKEBIN/curl" <<EOF
#!/bin/sh
destination=
url=
while [ "\$#" -gt 0 ]; do
    case "\$1" in
        --output)
            destination=\$2
            shift 2
            ;;
        --fail|--location|--silent|--show-error)
            shift
            ;;
        --retry|--connect-timeout)
            shift 2
            ;;
        *)
            url=\$1
            shift
            ;;
    esac
done
printf '%s\n' "\$url" >>"$CURL_LOG"
case "\${FAKE_RELEASE_MODE:-normal}:\$url" in
    missing:*)
        exit 22
        ;;
    bad:*latest/download*)
        source="$BAD_RELEASE"
        ;;
    wrong-root:*latest/download*)
        source="$WRONG_ROOT_RELEASE"
        ;;
    *:*/releases/latest/download/*)
        source="$LATEST_RELEASE"
        ;;
    *:*/releases/download/v1.3.0/*)
        source="$V130_RELEASE"
        ;;
    *)
        exit 22
        ;;
esac
cp "\$source/\${url##*/}" "\$destination"
EOF
chmod 0755 "$FAKEBIN/curl"

export MDVWB_ROOT="$ROOTFS"
export MDVWB_DPKG="$FAKEBIN/dpkg"
export MDVWB_LDCONFIG="$FAKEBIN/ldconfig"
export MDVWB_SYSTEMCTL="$FAKEBIN/systemctl"
export MDVWB_SERVICE_START_DELAY=0
export MDVWB_CURL="$FAKEBIN/curl"
export MDVWB_DOWNLOAD_DIR="$TEMPORARY/downloads"

sh -n "$ONLINE"
sh -n "$COMPAT"
sh "$ONLINE" --help >"$TEMPORARY/help"
grep -q '^MDVWB online installer$' "$TEMPORARY/help" ||
    fail "online help heading is missing"
grep -q -- '--keep-staging' "$TEMPORARY/help" ||
    fail "online help does not mention staging"

sh "$COMPAT" --help >"$TEMPORARY/compat-help"
grep -q '^MDVWB online installer$' "$TEMPORARY/compat-help" ||
    fail "compatibility wrapper does not call online installer"

printf 'background-one\n' >"$ROOTFS/var/www/fancoils/assets/user.png"

sh "$ONLINE" install >"$TEMPORARY/install-output"
grep -q '^MDVWB_RESULT=success$' "$TEMPORARY/install-output" ||
    fail "online install did not report success"
grep -q '^method=online$' "$TEMPORARY/install-output" ||
    fail "online method was not propagated"
grep -q '"version": "1.2.0"' \
    "$ROOTFS/var/lib/mdvwb/installation.json" ||
    fail "online install wrote the wrong version"
grep -q '"installMethod": "online"' \
    "$ROOTFS/var/lib/mdvwb/installation.json" ||
    fail "installation state does not record online method"
grep -q '/releases/latest/download/MDVWB-arm64-offline.tar.gz$' \
    "$CURL_LOG" || fail "latest release archive URL is wrong"

cat >"$ROOTFS/etc/mdvwb/buses.json" <<'EOF'
{
  "version": 1,
  "buses": [
    {"id":1,"enabled":true,"port":"/dev/ttyRS485-1","addresses":[1]}
  ]
}
EOF
printf 'background-two\n' >"$ROOTFS/var/www/fancoils/assets/user.png"

sh "$ONLINE" update --version v1.3.0 >"$TEMPORARY/update-output"
grep -q '^action=update$' "$TEMPORARY/update-output" ||
    fail "online update action is wrong"
grep -q '"version": "1.3.0"' \
    "$ROOTFS/var/lib/mdvwb/installation.json" ||
    fail "online update wrote the wrong version"
grep -q '"installMethod": "online"' \
    "$ROOTFS/var/lib/mdvwb/installation.json" ||
    fail "online update lost its method"
[ "$(cat "$ROOTFS/var/www/fancoils/assets/user.png")" = \
    "background-two" ] || fail "online update replaced uploaded asset"
grep -q '/releases/download/v1.3.0/MDVWB-arm64-offline.tar.gz$' \
    "$CURL_LOG" || fail "versioned release archive URL is wrong"

sh "$ONLINE" update --version 1.3.0 --force --dry-run --keep-staging \
    >"$TEMPORARY/keep-output"
staging=$(sed -n 's/^staging=//p' "$TEMPORARY/keep-output")
[ -n "$staging" ] && [ -d "$staging" ] ||
    fail "--keep-staging did not preserve the download directory"
grep -q '^DRY_RUN=complete$' "$TEMPORARY/keep-output" ||
    fail "online dry-run did not reach lifecycle engine"
rm -rf "$staging"

before_systemctl=$(wc -l <"$SYSTEMCTL_LOG")
export FAKE_RELEASE_MODE=bad
expect_code 2 sh "$ONLINE" update --force
after_systemctl=$(wc -l <"$SYSTEMCTL_LOG")
[ "$before_systemctl" -eq "$after_systemctl" ] ||
    fail "outer checksum failure changed services"
grep -q 'checksum does not match' "$TEMPORARY/stderr" ||
    fail "outer checksum failure has no explanation"

export FAKE_RELEASE_MODE=wrong-root
expect_code 2 sh "$ONLINE" update --force
grep -q 'unsafe or unexpected path' "$TEMPORARY/stderr" ||
    fail "unexpected archive root was not rejected"

export FAKE_RELEASE_MODE=missing
expect_code 22 sh "$ONLINE" update --force

unset FAKE_RELEASE_MODE
expect_code 2 sh "$ONLINE" --version 1.02.0
grep -q 'canonical MAJOR.MINOR.PATCH' "$TEMPORARY/stderr" ||
    fail "non-canonical release version was accepted"

expect_code 2 sh "$ONLINE" --repository invalid
grep -q 'owner/name' "$TEMPORARY/stderr" ||
    fail "invalid repository was accepted"

printf 'MDVWB online installer tests: OK\n'
