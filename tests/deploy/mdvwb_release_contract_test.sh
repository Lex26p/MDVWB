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

require_text "$VALIDATE_WORKFLOW" 'tests/deploy/mdvwb_release_contract_test.sh'
require_text "$VALIDATE_WORKFLOW" '".github/workflows/build-arm64-offline.yml"'
require_text "$VALIDATE_WORKFLOW" '"docs/**"'
require_text "$VALIDATE_WORKFLOW" '"README.md"'

if grep -Fq 'sha256sum dist/MDVWB-arm64-offline.tar.gz' \
    "$BUILD_WORKFLOW"; then
    fail "outer checksum must contain a portable basename, not dist/path"
fi

asset_count=$(sed -n '/ASSETS=(/,/)/p' "$BUILD_WORKFLOW" |
    grep -c '"dist/')

[ "$asset_count" -eq 5 ] ||
    fail "release publication must list exactly five required assets"

printf 'MDVWB release contract tests: OK (version %s)\n' "$VERSION"
