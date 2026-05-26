#!/usr/bin/env bash
set +e
INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"

echo "Stopping services..."
bash "$INSTALL_DIR/stop.sh" 2>/dev/null || true

echo "Removing boot hook..."
sed -i '/# commaview-hook/d; /commaview\/start.sh/d' /data/continue.sh 2>/dev/null || true

if [ -x "$INSTALL_DIR/scripts/revert_onroad_ui_export_patch.sh" ]; then
  echo "Reverting direct v2 onroad UI export transformer..."
  COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" bash "$INSTALL_DIR/scripts/revert_onroad_ui_export_patch.sh"
  revert_ec=$?
  if [ "$revert_ec" -ne 0 ]; then
    echo "WARN: direct v2 onroad UI export transformer revert failed with exit $revert_ec" >&2
  fi
else
  echo "WARN: direct v2 onroad UI export transformer revert helper missing" >&2
fi

echo "Removing files..."
rm -rf "$INSTALL_DIR"
echo "CommaView uninstalled"
