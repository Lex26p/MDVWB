#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INSTALL_METHOD=${MDVWB_INSTALL_METHOD:-offline}
COMMAND=${1:-install}

case "$INSTALL_METHOD" in
    offline|online|source)
        ;;
    *)
        echo "Unsupported MDVWB_INSTALL_METHOD: $INSTALL_METHOD" >&2
        exit 2
        ;;
esac

root_prefix()
{
    root=${MDVWB_ROOT:-/}
    case "$root" in
        /)
            printf '%s' ''
            ;;
        /*)
            printf '%s' "${root%/}"
            ;;
        *)
            echo "MDVWB_ROOT must be an absolute path: $root" >&2
            exit 2
            ;;
    esac
}

has_modbus_payload()
{
    [ -e "$SCRIPT_DIR/mdvwb-modbus" ] ||
        [ -e "$SCRIPT_DIR/modbus-profiles" ]
}

verify_modbus_payload()
{
    # Package format 1 existed before the Modbus runtime. Keep old package
    # fixtures and rollback archives valid when neither new payload component
    # is present, but reject a torn package containing only one component.
    has_modbus_payload || return 0

    [ -f "$SCRIPT_DIR/mdvwb-modbus" ] || {
        echo "Modbus runtime is missing from package: $SCRIPT_DIR/mdvwb-modbus" >&2
        exit 2
    }
    [ -x "$SCRIPT_DIR/mdvwb-modbus" ] || {
        echo "Modbus runtime is not executable: $SCRIPT_DIR/mdvwb-modbus" >&2
        exit 2
    }
    [ -r "$SCRIPT_DIR/modbus-profiles/vrf_add_controller.json" ] || {
        echo "Shipped Modbus profile is missing: $SCRIPT_DIR/modbus-profiles/vrf_add_controller.json" >&2
        exit 2
    }
}

contains_argument()
{
    expected=$1
    shift
    for argument in "$@"; do
        [ "$argument" != "$expected" ] || return 0
    done
    return 1
}

save_existing_modbus_payload()
{
    save_dir=$1
    prefix=$(root_prefix)
    lib_dir=$prefix/usr/local/lib/mdvwb

    mkdir -p "$save_dir"
    if [ -d "$lib_dir" ]; then
        : >"$save_dir/lib.present"
    fi
    if [ -e "$lib_dir/mdvwb-modbus" ]; then
        cp -p "$lib_dir/mdvwb-modbus" "$save_dir/mdvwb-modbus"
        : >"$save_dir/runtime.present"
    fi
    if [ -d "$lib_dir/modbus-profiles" ]; then
        cp -a "$lib_dir/modbus-profiles" "$save_dir/modbus-profiles"
        : >"$save_dir/profiles.present"
    fi
}

install_modbus_payload()
{
    prefix=$(root_prefix)
    lib_dir=$prefix/usr/local/lib/mdvwb
    profile_dir=$lib_dir/modbus-profiles

    install -d -m 0755 "$lib_dir" "$profile_dir"

    runtime_tmp=$lib_dir/.mdvwb-modbus.tmp.$$
    profile_tmp=$profile_dir/.vrf_add_controller.json.tmp.$$
    trap 'rm -f "$runtime_tmp" "$profile_tmp"' HUP INT TERM

    install -m 0755 "$SCRIPT_DIR/mdvwb-modbus" "$runtime_tmp"
    mv -f "$runtime_tmp" "$lib_dir/mdvwb-modbus"

    install -m 0644 \
        "$SCRIPT_DIR/modbus-profiles/vrf_add_controller.json" \
        "$profile_tmp"
    mv -f "$profile_tmp" "$profile_dir/vrf_add_controller.json"

    trap - HUP INT TERM
}

restore_modbus_payload()
{
    save_dir=$1
    prefix=$(root_prefix)
    lib_dir=$prefix/usr/local/lib/mdvwb

    if [ -f "$save_dir/runtime.present" ]; then
        install -d -m 0755 "$lib_dir"
        install -m 0755 "$save_dir/mdvwb-modbus" "$lib_dir/mdvwb-modbus"
    else
        rm -f "$lib_dir/mdvwb-modbus"
    fi

    rm -rf "$lib_dir/modbus-profiles"
    if [ -f "$save_dir/profiles.present" ]; then
        install -d -m 0755 "$lib_dir"
        cp -a "$save_dir/modbus-profiles" "$lib_dir/modbus-profiles"
    fi

    if [ ! -f "$save_dir/lib.present" ]; then
        rmdir "$lib_dir" 2>/dev/null || true
    fi
}

patch_setup_backup()
{
    backup=$1
    save_dir=$2
    [ -n "$backup" ] || return 0
    [ -d "$backup" ] || return 0

    backup_lib=$backup/files/usr/local/lib/mdvwb
    inventory=$backup/paths.tsv

    if [ ! -f "$save_dir/lib.present" ]; then
        rm -rf "$backup_lib"
        if [ -f "$inventory" ]; then
            temporary=$inventory.tmp.$$
            awk -F '\t' 'BEGIN { OFS="\t" }
                $2 == "usr/local/lib/mdvwb" { $1="absent" }
                { print }
            ' "$inventory" >"$temporary"
            mv -f "$temporary" "$inventory"
        fi
        return 0
    fi

    mkdir -p "$backup_lib"
    if [ -f "$save_dir/runtime.present" ]; then
        install -m 0755 "$save_dir/mdvwb-modbus" "$backup_lib/mdvwb-modbus"
    else
        rm -f "$backup_lib/mdvwb-modbus"
    fi

    rm -rf "$backup_lib/modbus-profiles"
    if [ -f "$save_dir/profiles.present" ]; then
        cp -a "$save_dir/modbus-profiles" "$backup_lib/modbus-profiles"
    fi
}

run_install_or_update()
{
    operation=$1
    shift

    verify_modbus_payload

    set -- "$operation" --package "$SCRIPT_DIR" --method "$INSTALL_METHOD" "$@"
    if ! has_modbus_payload || contains_argument --dry-run "$@"; then
        exec sh "$SCRIPT_DIR/mdvwb-setup" "$@"
    fi

    # Complete the setup engine's own preflight before touching installed
    # runtime files. This preserves its normal validation and version policy.
    sh "$SCRIPT_DIR/mdvwb-setup" "$@" --dry-run >/dev/null

    save_dir=${TMPDIR:-/tmp}/mdvwb-modbus-payload.$$
    output=${TMPDIR:-/tmp}/mdvwb-setup-output.$$
    rm -rf "$save_dir"
    trap 'rm -rf "$save_dir"; rm -f "$output"' EXIT HUP INT TERM

    save_existing_modbus_payload "$save_dir"
    install_modbus_payload

    status=0
    sh "$SCRIPT_DIR/mdvwb-setup" "$@" >"$output" 2>&1 || status=$?
    cat "$output"

    backup=$(sed -n 's/^BACKUP_CREATED path=//p' "$output" | tail -n 1)
    patch_setup_backup "$backup" "$save_dir"

    if [ "$status" -ne 0 ]; then
        restore_modbus_payload "$save_dir"
        exit "$status"
    fi

    rm -rf "$save_dir"
    rm -f "$output"
    trap - EXIT HUP INT TERM
}

case "$COMMAND" in
    install|update)
        [ "$#" -eq 0 ] || shift
        run_install_or_update "$COMMAND" "$@"
        ;;
    verify)
        shift
        verify_modbus_payload
        exec sh "$SCRIPT_DIR/mdvwb-setup" verify \
            --package "$SCRIPT_DIR" "$@"
        ;;
    --verify-only)
        shift
        verify_modbus_payload
        exec sh "$SCRIPT_DIR/mdvwb-setup" verify \
            --package "$SCRIPT_DIR" "$@"
        ;;
    version|--version|-V)
        [ "$#" -eq 0 ] || shift
        [ "$#" -eq 0 ] || {
            echo "version does not accept arguments." >&2
            exit 2
        }
        exec sh "$SCRIPT_DIR/mdvwb-setup" manifest \
            --manifest "$SCRIPT_DIR/manifest.json"
        ;;
    status)
        shift
        exec sh "$SCRIPT_DIR/mdvwb-setup" status --health "$@"
        ;;
    backup)
        shift
        exec sh "$SCRIPT_DIR/mdvwb-setup" backup "$@"
        ;;
    rollback)
        shift
        exec sh "$SCRIPT_DIR/mdvwb-setup" rollback "$@"
        ;;
    uninstall)
        shift
        exec sh "$SCRIPT_DIR/mdvwb-setup" uninstall "$@"
        ;;
    purge)
        shift
        exec sh "$SCRIPT_DIR/mdvwb-setup" purge "$@"
        ;;
    help|--help|-h)
        cat <<'HELP_EOF'
MDVWB offline package

Usage:
  ./offline-install.sh verify [--package-only]
  ./offline-install.sh version
  ./offline-install.sh status
  ./offline-install.sh install [options]
  ./offline-install.sh update [options]
  ./offline-install.sh backup [--backup-dir <directory>]
  ./offline-install.sh rollback [--backup <directory>]
      [--backup-dir <directory>]
  ./offline-install.sh uninstall --yes [options]
  ./offline-install.sh purge --yes [options]

Removal options:
  --dry-run
  --force
  --backup-dir <directory>
  --no-backup
  --keep-retained
  --remove-backups  (purge only)

Install and update options:
  --force
  --allow-downgrade
  --dry-run
  --backup-dir <directory>
  --no-backup

With no command, install is used. A leading option such as --dry-run is also
treated as an install option.
HELP_EOF
        ;;
    -*)
        run_install_or_update install "$@"
        ;;
    *)
        echo "Unknown offline command: $COMMAND" >&2
        echo "Run ./offline-install.sh --help for usage." >&2
        exit 2
        ;;
esac
