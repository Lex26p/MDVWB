#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
COMMAND=${1:-install}

case "$COMMAND" in
    install|update)
        [ "$#" -eq 0 ] || shift
        set -- "$COMMAND" --package "$SCRIPT_DIR" --method offline "$@"
        exec sh "$SCRIPT_DIR/mdvwb-setup" "$@"
        ;;
    verify)
        shift
        exec sh "$SCRIPT_DIR/mdvwb-setup" verify \
            --package "$SCRIPT_DIR" "$@"
        ;;
    --verify-only)
        shift
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
    help|--help|-h)
        cat <<'EOF'
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

Install and update options:
  --force
  --allow-downgrade
  --dry-run
  --backup-dir <directory>
  --no-backup

With no command, install is used. A leading option such as --dry-run is also
treated as an install option.
EOF
        ;;
    -*)
        set -- install --package "$SCRIPT_DIR" --method offline "$@"
        exec sh "$SCRIPT_DIR/mdvwb-setup" "$@"
        ;;
    *)
        echo "Unknown offline command: $COMMAND" >&2
        echo "Run ./offline-install.sh --help for usage." >&2
        exit 2
        ;;
esac
