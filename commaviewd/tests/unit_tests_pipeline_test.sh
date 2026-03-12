#!/usr/bin/env bash
set -euo pipefail

RUNNER="/home/pear/CommaView/commaviewd/scripts/run-unit-tests.sh"
PIPELINE="/home/pear/CommaView/commaviewd/scripts/run-verification.sh"
INTERFACE_GUARD="/home/pear/CommaView/commaviewd/scripts/upstream-interface-guard.sh"
BINARY_CONTRACT="/home/pear/CommaView/commaviewd/scripts/binary-contract-check.sh"
BUNDLE="/home/pear/CommaView/tools/release/comma4-build-bundle.sh"

for script in "$RUNNER" "$PIPELINE" "$INTERFACE_GUARD" "$BINARY_CONTRACT" "$BUNDLE"; do
  [ -x "$script" ] || { echo "FAIL: missing executable $script"; exit 1; }
done

"$RUNNER" --help >/dev/null
"$PIPELINE" --help >/dev/null
"$INTERFACE_GUARD" --help >/dev/null
"$BINARY_CONTRACT" --help >/dev/null
"$BUNDLE" --help >/dev/null

echo "PASS: verification pipeline scripts exist and support --help"
