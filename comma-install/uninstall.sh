#!/usr/bin/env bash
# CommaView remote uninstall helper.
# Usage: cat uninstall.sh | ssh comma@<comma-ip> bash
set -euo pipefail

INSTALL_DIR="/data/commaview"
CONTINUE_SH="/data/continue.sh"
MARKER="# commaview-hook"

echo "Stopping CommaView services..."
if [ -x "$INSTALL_DIR/stop.sh" ]; then
  bash "$INSTALL_DIR/stop.sh" 2>/dev/null || true
fi
pkill -f 'commaview-supervisor.sh' 2>/dev/null || true
pkill -f '/data/commaview/commaview-bridge' 2>/dev/null || true
pkill -f 'system/loggerd/encoderd --stream' 2>/dev/null || true

echo "Removing boot hook..."
if [ -f "$CONTINUE_SH" ]; then
  sed -i '/# commaview-hook/d; /commaview\/start.sh/d' "$CONTINUE_SH" 2>/dev/null || true
fi

echo "Removing CommaView files..."
rm -rf "$INSTALL_DIR"

echo "CommaView uninstalled"
