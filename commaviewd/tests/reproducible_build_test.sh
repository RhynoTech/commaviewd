#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="$ROOT/scripts/reproducible-build.sh"

[ -x "$SCRIPT" ] || { echo "FAIL: missing executable $SCRIPT"; exit 1; }
"$SCRIPT" --help >/dev/null

echo "PASS: reproducible build script exists and supports --help"
