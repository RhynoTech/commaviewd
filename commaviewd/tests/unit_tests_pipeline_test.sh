#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
RUNNER="$ROOT/scripts/run-unit-tests.sh"
PIPELINE="$ROOT/scripts/run-verification.sh"
INTERFACE_GUARD="$ROOT/scripts/upstream-interface-guard.sh"
INTERFACE_GUARD_CONTRACT="$ROOT/tests/upstream_interface_guard_transformer_test.sh"
DEVICE_TEST_WORKFLOW_CONTRACT="$ROOT/tests/device_test_workflow_contract_test.sh"
CI_WORKFLOW_CONTRACT="$ROOT/tests/ci_workflow_contract_test.sh"
BINARY_CONTRACT="$ROOT/scripts/binary-contract-check.sh"
HUD_LITE_CI_CONTRACT="$ROOT/tests/onroad_ui_export_ci_contract_test.sh"
HUD_LITE_PATCH_CONTRACT="$REPO_ROOT/comma4/tests/onroad_ui_export_patch_contract_test.sh"
LOCAL_DISCOVERY_CONTRACT="$ROOT/tests/local_discovery_contract_test.sh"
BUNDLE="$REPO_ROOT/tools/release/comma4-build-bundle.sh"

for script in "$RUNNER" "$PIPELINE" "$INTERFACE_GUARD" "$INTERFACE_GUARD_CONTRACT" "$BINARY_CONTRACT" "$HUD_LITE_CI_CONTRACT" "$HUD_LITE_PATCH_CONTRACT" "$LOCAL_DISCOVERY_CONTRACT" "$BUNDLE" "$DEVICE_TEST_WORKFLOW_CONTRACT" "$CI_WORKFLOW_CONTRACT"; do
  [ -x "$script" ] || { echo "FAIL: missing executable $script"; exit 1; }
done

"$RUNNER" --help >/dev/null
"$PIPELINE" --help >/dev/null
"$INTERFACE_GUARD" --help >/dev/null
"$INTERFACE_GUARD_CONTRACT" >/dev/null
"$DEVICE_TEST_WORKFLOW_CONTRACT" >/dev/null
"$CI_WORKFLOW_CONTRACT" >/dev/null
"$BINARY_CONTRACT" --help >/dev/null
"$HUD_LITE_CI_CONTRACT" >/dev/null
"$HUD_LITE_PATCH_CONTRACT" >/dev/null
"$LOCAL_DISCOVERY_CONTRACT" >/dev/null
"$BUNDLE" --help >/dev/null

grep -Fq 'local_discovery_contract_test.sh' "$RUNNER" || { echo "FAIL: run-unit-tests should execute local discovery contract"; exit 1; }
grep -Fq 'upstream_interface_guard_transformer_test.sh' "$RUNNER" || { echo "FAIL: run-unit-tests should execute upstream interface guard transformer contract"; exit 1; }
grep -Fq 'device_test_workflow_contract_test.sh' "$RUNNER" || { echo "FAIL: run-unit-tests should execute device-test workflow contract"; exit 1; }
grep -Fq 'ci_workflow_contract_test.sh' "$RUNNER" || { echo "FAIL: run-unit-tests should execute CI workflow contract"; exit 1; }
grep -Fq 'python3-pytest' "$REPO_ROOT/scripts/install-commaviewd-toolchain.sh" || { echo "FAIL: toolchain install should include python3-pytest"; exit 1; }

echo "PASS: verification pipeline scripts exist and support --help"
