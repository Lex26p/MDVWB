#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
SETUP="$ROOT_DIR/deploy/mdvwb-setup"
TEMPORARY=${TMPDIR:-/tmp}/mdvwb-setup-test.$$
trap 'rm -rf "$TEMPORARY"' EXIT HUP INT TERM
mkdir -p "$TEMPORARY"

fail()
{
    printf 'mdvwb-setup test failed: %s\n' "$*" >&2
    exit 1
}

make_manifest()
{
    path=$1
    version=$2
    commit=$3
    architecture=$4
    cat >"$path" <<EOF
{
  "packageFormat": 1,
  "product": "MDVWB",
  "version": "$version",
  "commit": "$commit",
  "architecture": "$architecture",
  "buildOs": "debian-bullseye",
  "builtAt": "2026-07-29T10:00:00Z"
}
EOF
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

COMMIT=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
STATE="$TEMPORARY/installation.json"
CURRENT="$TEMPORARY/current.json"
UPGRADE="$TEMPORARY/upgrade.json"
SAME="$TEMPORARY/same.json"
DOWNGRADE="$TEMPORARY/downgrade.json"
WRONG_ARCH="$TEMPORARY/wrong-arch.json"
BAD_VERSION="$TEMPORARY/bad-version.json"
DUPLICATE="$TEMPORARY/duplicate.json"

make_manifest "$CURRENT" 1.2.0 "$COMMIT" arm64
make_manifest "$UPGRADE" 1.3.0 "$COMMIT" arm64
make_manifest "$SAME" 1.2.0 "$COMMIT" arm64
make_manifest "$DOWNGRADE" 1.1.9 "$COMMIT" arm64
make_manifest "$WRONG_ARCH" 1.3.0 "$COMMIT" amd64
make_manifest "$BAD_VERSION" 1.02.0 "$COMMIT" arm64
cat >"$DUPLICATE" <<EOF
{
  "packageFormat": 1,
  "product": "MDVWB",
  "version": "1.2.0",
  "version": "1.3.0",
  "commit": "$COMMIT",
  "architecture": "arm64",
  "buildOs": "debian-bullseye",
  "builtAt": "2026-07-29T10:00:00Z"
}
EOF

sh -n "$SETUP"

sh "$SETUP" manifest --manifest "$CURRENT" >"$TEMPORARY/manifest-output"
grep -q '^PACKAGE_OK product=MDVWB version=1.2.0$' "$TEMPORARY/manifest-output" ||
    fail "manifest summary is incorrect"

sh "$SETUP" check-version --manifest "$CURRENT" --state "$STATE" >"$TEMPORARY/install-plan"
grep -q '^ACTION=install current=none target=1.2.0 architecture=arm64$' "$TEMPORARY/install-plan" ||
    fail "fresh install plan is incorrect"

sh "$SETUP" write-state --manifest "$CURRENT" --method offline --state "$STATE" \
    >"$TEMPORARY/write-output"
[ -s "$STATE" ] || fail "installation state was not written"

sh "$SETUP" status --state "$STATE" >"$TEMPORARY/status-output"
grep -q '^INSTALLED product=MDVWB version=1.2.0$' "$TEMPORARY/status-output" ||
    fail "status does not show installed version"
grep -q '^installMethod=offline$' "$TEMPORARY/status-output" ||
    fail "status does not show installation method"

sh "$SETUP" check-version --manifest "$UPGRADE" --state "$STATE" >"$TEMPORARY/update-plan"
grep -q '^ACTION=update current=1.2.0 target=1.3.0 architecture=arm64$' "$TEMPORARY/update-plan" ||
    fail "upgrade plan is incorrect"

expect_code 4 sh "$SETUP" check-version --manifest "$SAME" --state "$STATE"
grep -q 'already installed' "$TEMPORARY/stderr" ||
    fail "same-version rejection has no explanation"

sh "$SETUP" check-version --manifest "$SAME" --state "$STATE" --force \
    >"$TEMPORARY/repair-plan"
grep -q '^ACTION=repair current=1.2.0 target=1.2.0 architecture=arm64$' "$TEMPORARY/repair-plan" ||
    fail "repair plan is incorrect"

expect_code 5 sh "$SETUP" check-version --manifest "$DOWNGRADE" --state "$STATE"
grep -q 'requires --allow-downgrade' "$TEMPORARY/stderr" ||
    fail "downgrade rejection has no explanation"

sh "$SETUP" check-version --manifest "$DOWNGRADE" --state "$STATE" \
    --allow-downgrade >"$TEMPORARY/downgrade-plan"
grep -q '^ACTION=downgrade current=1.2.0 target=1.1.9 architecture=arm64$' \
    "$TEMPORARY/downgrade-plan" || fail "downgrade plan is incorrect"

expect_code 2 sh "$SETUP" manifest --manifest "$WRONG_ARCH"
grep -q 'unsupported package architecture' "$TEMPORARY/stderr" ||
    fail "wrong architecture was not rejected"

expect_code 2 sh "$SETUP" manifest --manifest "$BAD_VERSION"
grep -q 'canonical MAJOR.MINOR.PATCH' "$TEMPORARY/stderr" ||
    fail "non-canonical version was not rejected"

expect_code 2 sh "$SETUP" manifest --manifest "$DUPLICATE"
grep -q "field 'version' must occur exactly once" "$TEMPORARY/stderr" ||
    fail "duplicate manifest field was not rejected"

expect_code 3 sh "$SETUP" status --state "$TEMPORARY/missing-state.json"
grep -q '^NOT_INSTALLED state=' "$TEMPORARY/stdout" ||
    fail "missing installation status is incorrect"

printf 'MDVWB installation metadata tests: OK\n'
