#!/usr/bin/env bash
set -euo pipefail

RUNNER="/home/pear/CommaView/commaviewd/scripts/run-unit-tests.sh"
PIPELINE="/home/pear/CommaView/commaviewd/scripts/run-verification.sh"

[ -x "$RUNNER" ] || { echo "FAIL: missing executable $RUNNER"; exit 1; }
[ -x "$PIPELINE" ] || { echo "FAIL: missing executable $PIPELINE"; exit 1; }
"$RUNNER" --help >/dev/null
"$PIPELINE" --help >/dev/null

echo "PASS: unit test + verification pipeline scripts exist and support --help"
