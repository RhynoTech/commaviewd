#!/usr/bin/env bash
set +e
INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
FORCE_OFFROAD=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --force-offroad) FORCE_OFFROAD=1; shift ;;
    -h|--help) echo "Usage: uninstall.sh [--force-offroad]"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

revert_args=()
if [ "$FORCE_OFFROAD" = "1" ]; then
  revert_args+=(--force-offroad)
fi
revert_helper="$INSTALL_DIR/scripts/revert_onroad_ui_export_patch.sh"

if [ -x "$revert_helper" ]; then
  echo "Reverting direct v2 onroad UI export transformer..."
  COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" bash "$revert_helper" "${revert_args[@]}"
  revert_ec=$?
  if [ "$revert_ec" -ne 0 ]; then
    echo "ERROR: uninstall aborted before stopping services or removing boot hook; direct v2 onroad UI export transformer revert failed with exit $revert_ec; preserving $INSTALL_DIR for recovery" >&2
    exit "$revert_ec"
  fi
else
  echo "ERROR: direct v2 onroad UI export transformer revert helper missing; uninstall aborted before stopping services or removing files; preserving $INSTALL_DIR for recovery" >&2
  exit 1
fi

echo "Stopping services..."
bash "$INSTALL_DIR/stop.sh" 2>/dev/null || true

echo "Removing boot hook..."
sed -i '/# commaview-hook/d; /commaview\/start.sh/d' /data/continue.sh 2>/dev/null || true

echo "Removing files..."
rm -rf "$INSTALL_DIR"
echo "CommaView uninstalled"
