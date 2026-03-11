#!/usr/bin/env bash
set -euo pipefail

SCRIPT="/home/pear/CommaView/commaviewd/scripts/reproducible-build.sh"

[ -x "$SCRIPT" ] || { echo "FAIL: missing executable $SCRIPT"; exit 1; }
"$SCRIPT" --help >/dev/null

echo "PASS: reproducible build script exists and supports --help"
