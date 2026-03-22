#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
RUNNER="$ROOT/scripts/run-unit-tests.sh"
PIPELINE="$ROOT/scripts/run-verification.sh"
INTERFACE_GUARD="$ROOT/scripts/upstream-interface-guard.sh"
BINARY_CONTRACT="$ROOT/scripts/binary-contract-check.sh"
HUD_LITE_CI_CONTRACT="$ROOT/tests/hud_lite_ci_contract_test.sh"
HUD_LITE_PATCH_CONTRACT="$REPO_ROOT/comma4/tests/hud_lite_patch_contract_test.sh"
BUNDLE="$REPO_ROOT/tools/release/comma4-build-bundle.sh"

for script in "$RUNNER" "$PIPELINE" "$INTERFACE_GUARD" "$BINARY_CONTRACT" "$HUD_LITE_CI_CONTRACT" "$HUD_LITE_PATCH_CONTRACT" "$BUNDLE"; do
  [ -x "$script" ] || { echo "FAIL: missing executable $script"; exit 1; }
done

"$RUNNER" --help >/dev/null
"$PIPELINE" --help >/dev/null
"$INTERFACE_GUARD" --help >/dev/null
"$BINARY_CONTRACT" --help >/dev/null
"$HUD_LITE_CI_CONTRACT" >/dev/null
"$HUD_LITE_PATCH_CONTRACT" >/dev/null
"$BUNDLE" --help >/dev/null

echo "PASS: verification pipeline scripts exist and support --help"
