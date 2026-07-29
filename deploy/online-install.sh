#!/bin/sh
set -eu

PROGRAM=${0##*/}
DEFAULT_REPOSITORY=${MDVWB_GITHUB_REPOSITORY:-Lex26p/MDVWB}
DEFAULT_ASSET=MDVWB-arm64-offline.tar.gz
OPERATION=install
VERSION=latest
REPOSITORY=$DEFAULT_REPOSITORY
RELEASE_BASE_URL=${MDVWB_RELEASE_BASE_URL:-}
KEEP_STAGING=0
FORCE=0
ALLOW_DOWNGRADE=0
DRY_RUN=0
NO_BACKUP=0
BACKUP_DIR=
STAGING_PARENT=${MDVWB_DOWNLOAD_DIR:-${TMPDIR:-/tmp}}
CURL_PROGRAM=${MDVWB_CURL:-}
WGET_PROGRAM=${MDVWB_WGET:-}
TAR_PROGRAM=${MDVWB_TAR:-tar}

fail()
{
    code=$1
    shift
    printf '%s: %s\n' "$PROGRAM" "$*" >&2
    exit "$code"
}

usage()
{
    cat <<'EOF'
MDVWB online installer

Usage:
  ./online-install.sh [install|update] [options]

Release selection:
  --version <latest|MAJOR.MINOR.PATCH>
  --repository <owner/repository>
  --release-base-url <URL>

Lifecycle options:
  --force
  --allow-downgrade
  --dry-run
  --backup-dir <directory>
  --no-backup

Download options:
  --keep-staging
  --download-dir <directory>

The default repository is Lex26p/MDVWB and the default release is latest.
The installer downloads the ARM64 offline package and its SHA-256 file,
validates the archive, then runs the common MDVWB lifecycle engine.
EOF
}

require_command()
{
    command -v "$1" >/dev/null 2>&1 ||
        fail 2 "required command is not available: $1"
}

validate_repository()
{
    repository=$1
    case "$repository" in
        */*)
            owner=${repository%%/*}
            name=${repository#*/}
            ;;
        *)
            fail 2 "repository must use owner/name form: $repository"
            ;;
    esac

    [ -n "$owner" ] && [ -n "$name" ] && [ "$name" = "${name%%/*}" ] ||
        fail 2 "repository must use owner/name form: $repository"

    case "$owner$name" in
        *[!A-Za-z0-9_.-]*)
            fail 2 "repository contains unsupported characters: $repository"
            ;;
    esac
}

validate_version()
{
    version=$1
    [ "$version" = latest ] && return 0

    printf '%s\n' "$version" | awk -F. '
        NF != 3 { exit 1 }
        {
            for (field = 1; field <= 3; ++field) {
                if ($field !~ /^[0-9]+$/) {
                    exit 1
                }
                if (length($field) > 1 && substr($field, 1, 1) == "0") {
                    exit 1
                }
            }
        }
    ' || fail 2 "version must be latest or canonical MAJOR.MINOR.PATCH: $version"
}

validate_absolute_directory()
{
    directory=$1
    description=$2
    case "$directory" in
        /*)
            ;;
        *)
            fail 2 "$description must be an absolute path: $directory"
            ;;
    esac
}

select_downloader()
{
    if [ -n "$CURL_PROGRAM" ]; then
        require_command "$CURL_PROGRAM"
        DOWNLOADER=curl
        return
    fi
    if [ -n "$WGET_PROGRAM" ]; then
        require_command "$WGET_PROGRAM"
        DOWNLOADER=wget
        return
    fi
    if command -v curl >/dev/null 2>&1; then
        CURL_PROGRAM=curl
        DOWNLOADER=curl
        return
    fi
    if command -v wget >/dev/null 2>&1; then
        WGET_PROGRAM=wget
        DOWNLOADER=wget
        return
    fi
    fail 2 "curl or wget is required for online installation"
}

download_file()
{
    url=$1
    destination=$2

    printf 'DOWNLOAD url=%s\n' "$url"
    if [ "$DOWNLOADER" = curl ]; then
        "$CURL_PROGRAM" \
            --fail \
            --location \
            --silent \
            --show-error \
            --retry 3 \
            --connect-timeout 20 \
            --output "$destination" \
            "$url"
    else
        "$WGET_PROGRAM" \
            --quiet \
            --timeout=20 \
            --tries=3 \
            --output-document="$destination" \
            "$url"
    fi

    [ -s "$destination" ] ||
        fail 2 "downloaded file is empty: $url"
}

verify_release_checksum()
{
    archive=$1
    checksum_file=$2
    asset=$3

    line_count=$(awk 'NF && $1 !~ /^#/ { count++ } END { print count + 0 }' \
        "$checksum_file")
    [ "$line_count" -eq 1 ] ||
        fail 2 "release checksum file must contain exactly one entry"

    expected=$(awk 'NF && $1 !~ /^#/ { print $1 }' "$checksum_file")
    listed=$(awk 'NF && $1 !~ /^#/ { print $2 }' "$checksum_file")
    listed=${listed#\*}

    case "$expected" in
        *[!0-9A-Fa-f]*|"")
            fail 2 "release checksum is not a hexadecimal SHA-256 value"
            ;;
    esac
    [ "${#expected}" -eq 64 ] ||
        fail 2 "release checksum must contain 64 hexadecimal characters"

    [ "${listed##*/}" = "$asset" ] ||
        fail 2 "release checksum references unexpected file: $listed"

    actual=$(sha256sum "$archive" | awk '{print $1}')
    [ "$(printf '%s' "$actual" | tr 'A-F' 'a-f')" = \
      "$(printf '%s' "$expected" | tr 'A-F' 'a-f')" ] ||
        fail 2 "downloaded release archive checksum does not match"
}

validate_archive()
{
    archive=$1
    listing=$2
    verbose_listing=$3

    "$TAR_PROGRAM" -tzf "$archive" >"$listing"
    [ -s "$listing" ] || fail 2 "release archive is empty"

    awk '
        /^\// { exit 1 }
        /(^|\/)\.\.(\/|$)/ { exit 1 }
        $0 !~ /^MDVWB-arm64(\/|$)/ { exit 1 }
    ' "$listing" ||
        fail 2 "release archive contains an unsafe or unexpected path"

    "$TAR_PROGRAM" -tvzf "$archive" >"$verbose_listing"
    awk '
        {
            type = substr($1, 1, 1)
            if (type != "-" && type != "d") {
                exit 1
            }
        }
    ' "$verbose_listing" ||
        fail 2 "release archive contains links or special files"
}

cleanup()
{
    status=$?
    if [ -n "${STAGING:-}" ]; then
        if [ "$KEEP_STAGING" -eq 1 ]; then
            printf 'staging=%s\n' "$STAGING"
        else
            rm -rf "$STAGING"
        fi
    fi
    exit "$status"
}

if [ "$#" -gt 0 ]; then
    case "$1" in
        install|update)
            OPERATION=$1
            shift
            ;;
    esac
fi

while [ "$#" -gt 0 ]; do
    case "$1" in
        --version)
            [ "$#" -ge 2 ] || fail 2 "--version requires a value"
            VERSION=${2#v}
            shift 2
            ;;
        --repository)
            [ "$#" -ge 2 ] || fail 2 "--repository requires owner/name"
            REPOSITORY=$2
            shift 2
            ;;
        --release-base-url)
            [ "$#" -ge 2 ] || fail 2 "--release-base-url requires a URL"
            RELEASE_BASE_URL=$2
            shift 2
            ;;
        --force)
            FORCE=1
            shift
            ;;
        --allow-downgrade)
            ALLOW_DOWNGRADE=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
            shift
            ;;
        --no-backup)
            NO_BACKUP=1
            shift
            ;;
        --backup-dir)
            [ "$#" -ge 2 ] || fail 2 "--backup-dir requires a directory"
            BACKUP_DIR=$2
            shift 2
            ;;
        --keep-staging)
            KEEP_STAGING=1
            shift
            ;;
        --download-dir)
            [ "$#" -ge 2 ] || fail 2 "--download-dir requires a directory"
            STAGING_PARENT=$2
            shift 2
            ;;
        help|--help|-h)
            usage
            exit 0
            ;;
        *)
            fail 2 "unknown online installer argument: $1"
            ;;
    esac
done

validate_repository "$REPOSITORY"
validate_version "$VERSION"
validate_absolute_directory "$STAGING_PARENT" "download directory"
[ -d "$STAGING_PARENT" ] ||
    fail 2 "download directory does not exist: $STAGING_PARENT"

require_command awk
require_command basename
require_command grep
require_command mktemp
require_command sed
require_command sha256sum
require_command tr
require_command "$TAR_PROGRAM"
select_downloader

if [ -z "$RELEASE_BASE_URL" ]; then
    RELEASE_BASE_URL="https://github.com/$REPOSITORY"
fi
RELEASE_BASE_URL=${RELEASE_BASE_URL%/}

if [ "$VERSION" = latest ]; then
    RELEASE_URL="$RELEASE_BASE_URL/releases/latest/download"
    RELEASE_LABEL=latest
else
    RELEASE_URL="$RELEASE_BASE_URL/releases/download/v$VERSION"
    RELEASE_LABEL="v$VERSION"
fi

ASSET=$DEFAULT_ASSET
CHECKSUM_ASSET=$ASSET.sha256
STAGING=$(mktemp -d "$STAGING_PARENT/mdvwb-online.XXXXXX")
trap cleanup 0
trap 'exit 130' HUP INT TERM

ARCHIVE="$STAGING/$ASSET"
CHECKSUM="$STAGING/$CHECKSUM_ASSET"
LISTING="$STAGING/archive.list"
VERBOSE_LISTING="$STAGING/archive.verbose"

printf '%s\n' \
    "ONLINE_SOURCE repository=$REPOSITORY release=$RELEASE_LABEL" \
    "asset=$ASSET"

download_file "$RELEASE_URL/$ASSET" "$ARCHIVE"
download_file "$RELEASE_URL/$CHECKSUM_ASSET" "$CHECKSUM"

printf '[online 1/4] Verify release checksum\n'
verify_release_checksum "$ARCHIVE" "$CHECKSUM" "$ASSET"

printf '[online 2/4] Validate release archive\n'
validate_archive "$ARCHIVE" "$LISTING" "$VERBOSE_LISTING"

printf '[online 3/4] Extract release package\n'
"$TAR_PROGRAM" -xzf "$ARCHIVE" -C "$STAGING"
PACKAGE="$STAGING/MDVWB-arm64"
[ -d "$PACKAGE" ] || fail 2 "release archive does not contain MDVWB-arm64"

printf '[online 4/4] Verify package and run lifecycle engine\n'
sh "$PACKAGE/offline-install.sh" verify --package-only

set -- "$OPERATION"
[ "$FORCE" -eq 0 ] || set -- "$@" --force
[ "$ALLOW_DOWNGRADE" -eq 0 ] || set -- "$@" --allow-downgrade
[ "$DRY_RUN" -eq 0 ] || set -- "$@" --dry-run
[ "$NO_BACKUP" -eq 0 ] || set -- "$@" --no-backup
[ -z "$BACKUP_DIR" ] || set -- "$@" --backup-dir "$BACKUP_DIR"

MDVWB_INSTALL_METHOD=online \
    sh "$PACKAGE/offline-install.sh" "$@"
