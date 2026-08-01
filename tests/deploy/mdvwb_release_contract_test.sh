#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
CMAKE="$ROOT_DIR/CMakeLists.txt"
README="$ROOT_DIR/README.md"
INSTALLATION="$ROOT_DIR/docs/INSTALLATION.md"
CHECKLIST="$ROOT_DIR/docs/RELEASE_CHECKLIST.md"
BUILD_WORKFLOW="$ROOT_DIR/.github/workflows/build-arm64-offline.yml"
VALIDATE_WORKFLOW="$ROOT_DIR/.github/workflows/validate.yml"
ONLINE="$ROOT_DIR/deploy/online-install.sh"
OFFLINE="$ROOT_DIR/deploy/offline-install.sh"
SOURCE_INSTALLER="$ROOT_DIR/deploy/install-from-source.sh"
MANIFEST="$ROOT_DIR/deploy/package-manifest.json.in"

fail()
{
    printf 'mdvwb release contract test failed: %s\n' "$*" >&2
    exit 1
}

require_text()
{
    file=$1
    text=$2
    grep -Fq -- "$text" "$file" ||
        fail "missing '$text' in ${file#"$ROOT_DIR/"}"
}

VERSION=$(sed -n \
    's/^project(MDVWB VERSION \([0-9][0-9.]*\) LANGUAGES CXX)$/\1/p' \
    "$CMAKE")
[ -n "$VERSION" ] || fail "project version is missing"

printf '%s\n' "$VERSION" | awk -F. '
    NF != 3 { exit 1 }
    {
        for (field = 1; field <= 3; ++field) {
            if ($field !~ /^(0|[1-9][0-9]*)$/) {
                exit 1
            }
        }
    }
' || fail "project version is not canonical MAJOR.MINOR.PATCH"

[ "$VERSION" = "1.3.0" ] ||
    fail "release step expects version 1.3.0, got $VERSION"

require_text "$README" "Current project version: **$VERSION**."
require_text "$INSTALLATION" "MDVWB $VERSION:"
require_text "$CHECKLIST" "MDVWB $VERSION release checklist"
require_text "$CHECKLIST" "tag: v$VERSION"

require_text "$MANIFEST" '"version": "@MDVWB_VERSION@"'
require_text "$ONLINE" 'DEFAULT_ASSET=MDVWB-arm64-offline.tar.gz'

require_text "$BUILD_WORKFLOW" 'tags:'
require_text "$BUILD_WORKFLOW" '"v*.*.*"'
require_text "$BUILD_WORKFLOW" 'contents: write'
require_text "$BUILD_WORKFLOW" 'test "$GITHUB_REF_NAME" = "v$PROJECT_VERSION"'
require_text "$BUILD_WORKFLOW" 'MDVWB-arm64-offline.tar.gz'
require_text "$BUILD_WORKFLOW" 'MDVWB-arm64-offline.tar.gz.sha256'
require_text "$BUILD_WORKFLOW" 'MDVWB-release-assets.sha256'
require_text "$BUILD_WORKFLOW" 'dist/online-install.sh'
require_text "$BUILD_WORKFLOW" 'dist/install_wirenboard.sh'
require_text "$BUILD_WORKFLOW" 'gh release create'
require_text "$BUILD_WORKFLOW" '--verify-tag'
require_text "$BUILD_WORKFLOW" '--generate-notes'
require_text "$BUILD_WORKFLOW" '--prerelease'
require_text "$BUILD_WORKFLOW" 'gh release upload'
require_text "$BUILD_WORKFLOW" '--clobber'
require_text "$BUILD_WORKFLOW" 'Verify packaged runtime on Debian 11 ARM64'
require_text "$BUILD_WORKFLOW" 'libmosquitto1'
require_text "$BUILD_WORKFLOW" 'sha256sum -c "$MDVWB_RELEASE_ARCHIVE_CHECKSUM"'
require_text "$BUILD_WORKFLOW" 'tar -C "$VERIFY_ROOT" -xzf "$MDVWB_RELEASE_ARCHIVE"'
require_text "$BUILD_WORKFLOW" 'sh ./offline-install.sh verify --package-only'
require_text "$BUILD_WORKFLOW" 'grep -F "version=$PROJECT_VERSION"'

# The live Modbus runtime and its reviewed production profile must travel with
# every official package. CMake install rules alone do not populate the manual
# release archive used by this project.
require_text "$BUILD_WORKFLOW" 'out/build/arm64-release/mdvwb-modbus'
require_text "$BUILD_WORKFLOW" 'dist/MDVWB-arm64/mdvwb-modbus'
require_text "$BUILD_WORKFLOW" 'profiles/modbus/vrf_add_controller.json'
require_text "$BUILD_WORKFLOW" 'dist/MDVWB-arm64/modbus-profiles/vrf_add_controller.json'
require_text "$BUILD_WORKFLOW" 'test -x ./mdvwb-modbus'
require_text "$BUILD_WORKFLOW" 'test -r ./modbus-profiles/vrf_add_controller.json'

require_text "$SOURCE_INSTALLER" '"$BUILD_DIR/mdvwb-modbus" "$STAGING/mdvwb-modbus"'
require_text "$SOURCE_INSTALLER" '"$SOURCE_DIR/profiles/modbus/vrf_add_controller.json"'
require_text "$SOURCE_INSTALLER" 'MDVWB_INSTALL_METHOD=source sh "$STAGING/offline-install.sh"'

require_text "$OFFLINE" 'verify_modbus_payload'
require_text "$OFFLINE" 'install_modbus_payload'
require_text "$OFFLINE" 'patch_setup_backup'
require_text "$OFFLINE" 'modbus-profiles/vrf_add_controller.json'

require_text "$VALIDATE_WORKFLOW" 'tests/deploy/mdvwb_release_contract_test.sh'
require_text "$VALIDATE_WORKFLOW" '".github/workflows/build-arm64-offline.yml"'
require_text "$VALIDATE_WORKFLOW" '"docs/**"'
require_text "$VALIDATE_WORKFLOW" '"README.md"'

if grep -Fq 'sha256sum dist/MDVWB-arm64-offline.tar.gz' \
    "$BUILD_WORKFLOW"; then
    fail "outer checksum must contain a portable basename, not dist/path"
fi

if grep -Fq 'sh dist/MDVWB-arm64/offline-install.sh verify --package-only' \
    "$BUILD_WORKFLOW"; then
    fail "packaged ARM64 binaries must not run on the Ubuntu host"
fi

asset_count=$(sed -n '/ASSETS=(/,/)/p' "$BUILD_WORKFLOW" |
    grep -c '"dist/')

[ "$asset_count" -eq 5 ] ||
    fail "release publication must list exactly five required assets"


test_modbus_payload_transaction()
{
    temporary=$(mktemp -d)
    package=$temporary/package
    root=$temporary/root
    mkdir -p \
        "$package/modbus-profiles" \
        "$root/usr/local/lib/mdvwb/modbus-profiles"

    cp "$OFFLINE" "$package/offline-install.sh"
    cat >"$package/mdvwb-modbus" <<'RUNTIME_EOF'
#!/bin/sh
printf 'new runtime\n'
RUNTIME_EOF
    chmod 0755 "$package/mdvwb-modbus"
    printf '{"new":true}\n' \
        >"$package/modbus-profiles/vrf_add_controller.json"

    cat >"$package/mdvwb-setup" <<'SETUP_EOF'
#!/bin/sh
set -eu
dry_run=0
for argument in "$@"; do
    [ "$argument" != --dry-run ] || dry_run=1
done
[ "$dry_run" -eq 0 ] || exit 0

root=${MDVWB_ROOT%/}
backup="$root/var/backups/mdvwb/fake-backup"
rm -rf "$backup"
mkdir -p "$backup/files/usr/local/lib"
if [ -d "$root/usr/local/lib/mdvwb" ]; then
    cp -a "$root/usr/local/lib/mdvwb" \
        "$backup/files/usr/local/lib/mdvwb"
    printf 'present\tusr/local/lib/mdvwb\n' >"$backup/paths.tsv"
else
    printf 'absent\tusr/local/lib/mdvwb\n' >"$backup/paths.tsv"
fi
touch "$backup/backup.conf" "$backup/COMPLETE" "$backup/services.tsv"
printf 'BACKUP_CREATED path=%s\n' "$backup"
[ "${FAKE_SETUP_FAIL:-0}" -eq 0 ] || exit "$FAKE_SETUP_FAIL"
SETUP_EOF
    chmod 0755 "$package/mdvwb-setup"

    cat >"$root/usr/local/lib/mdvwb/mdvwb-modbus" <<'OLD_EOF'
#!/bin/sh
printf 'old runtime\n'
OLD_EOF
    chmod 0755 "$root/usr/local/lib/mdvwb/mdvwb-modbus"
    printf '{"old":true}\n' \
        >"$root/usr/local/lib/mdvwb/modbus-profiles/vrf_add_controller.json"

    MDVWB_ROOT="$root" sh "$package/offline-install.sh" \
        install --no-backup >/dev/null

    grep -Fq 'new runtime' \
        "$root/usr/local/lib/mdvwb/mdvwb-modbus" ||
        fail "Modbus runtime payload was not installed"
    grep -Fq 'old runtime' \
        "$root/var/backups/mdvwb/fake-backup/files/usr/local/lib/mdvwb/mdvwb-modbus" ||
        fail "lifecycle backup did not preserve the previous Modbus runtime"
    grep -Fq '"old":true' \
        "$root/var/backups/mdvwb/fake-backup/files/usr/local/lib/mdvwb/modbus-profiles/vrf_add_controller.json" ||
        fail "lifecycle backup did not preserve the previous Modbus profile"

    cat >"$root/usr/local/lib/mdvwb/mdvwb-modbus" <<'OLD2_EOF'
#!/bin/sh
printf 'old runtime 2\n'
OLD2_EOF
    chmod 0755 "$root/usr/local/lib/mdvwb/mdvwb-modbus"
    printf '{"old":2}\n' \
        >"$root/usr/local/lib/mdvwb/modbus-profiles/vrf_add_controller.json"

    set +e
    FAKE_SETUP_FAIL=7 MDVWB_ROOT="$root" \
        sh "$package/offline-install.sh" update --no-backup \
        >/dev/null 2>&1
    status=$?
    set -e
    [ "$status" -eq 7 ] ||
        fail "failed setup did not preserve its exit code"
    grep -Fq 'old runtime 2' \
        "$root/usr/local/lib/mdvwb/mdvwb-modbus" ||
        fail "failed setup did not restore the previous Modbus runtime"
    grep -Fq '"old":2' \
        "$root/usr/local/lib/mdvwb/modbus-profiles/vrf_add_controller.json" ||
        fail "failed setup did not restore the previous Modbus profile"

    rm -rf "$temporary"
}

test_modbus_payload_transaction

printf 'MDVWB release contract tests: OK (version %s)\n' "$VERSION"
