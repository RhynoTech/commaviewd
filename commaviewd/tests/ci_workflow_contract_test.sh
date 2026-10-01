#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
WORKFLOW="$REPO_ROOT/.github/workflows/commaviewd-ci.yml"

assert_file() {
  [[ -f "$1" ]] || { echo "FAIL: missing $1" >&2; exit 1; }
}

assert_contains() {
  local needle="$1"
  local file="$2"
  local message="$3"
  grep -Fq -- "$needle" "$file" || { echo "FAIL: $message" >&2; exit 1; }
}

TARGETS="$REPO_ROOT/ci/targets.json"
PLAN="$REPO_ROOT/ci/plan-targets.py"

assert_file "$WORKFLOW"
assert_file "$TARGETS"
assert_file "$PLAN"
python3 -m json.tool "$TARGETS" >/dev/null || { echo "FAIL: ci/targets.json is not valid JSON" >&2; exit 1; }

# The targets (ci/targets.json, planned by ci/plan-targets.py).
assert_contains '"name": "openpilot-release-mici"' "$TARGETS" "commaviewd CI should validate openpilot release-mici"
assert_contains '"upstream_ref": "release-mici"' "$TARGETS" "commaviewd CI should include MICI release refs"
assert_contains '"name": "openpilot-release-tici"' "$TARGETS" "commaviewd CI should validate openpilot release-tici"
assert_contains '"upstream_ref": "release-tici"' "$TARGETS" "commaviewd CI should include openpilot TICI release ref"
assert_contains '"run_verification": false' "$TARGETS" "legacy openpilot release-tici should be patch-applicability only"
assert_contains '"upstream_repo": "commaai/openpilot"' "$TARGETS" "commaviewd CI should include commaai/openpilot"
assert_contains '"name": "sunnypilot-release-mici"' "$TARGETS" "commaviewd CI should validate sunnypilot release-mici"
assert_contains '"name": "sunnypilot-release-tizi"' "$TARGETS" "commaviewd CI should validate sunnypilot release-tizi"
assert_contains '"upstream_ref": "release-tizi"' "$TARGETS" "commaviewd CI should include sunnypilot TIZI release ref"
assert_contains '"upstream_repo": "sunnypilot/sunnypilot"' "$TARGETS" "commaviewd CI should include sunnypilot/sunnypilot"
assert_contains '"name": "openpilot-release-tizi"' "$TARGETS" "commaviewd CI should validate openpilot release-tizi"
assert_contains '"name": "openpilot-release-chestnut-mici"' "$TARGETS" "commaviewd CI should validate openpilot release-chestnut on MICI"
assert_contains '"name": "openpilot-release-chestnut-tizi"' "$TARGETS" "commaviewd CI should validate openpilot release-chestnut on TIZI"
assert_contains '"upstream_ref": "release-chestnut"' "$TARGETS" "commaviewd CI should include the openpilot chestnut release ref"
assert_contains '"name": "sunnypilot-release-pin"' "$TARGETS" "commaviewd CI should build the pinned sunnypilot release source"
assert_contains 'upstream-refs.env' "$PLAN" "commaviewd CI should resolve the release pin from ci/upstream-refs.env"
assert_contains 'COMMAVIEWD_RELEASE_SUNNYPILOT_REF' "$PLAN" "commaviewd CI should resolve the release pin from ci/upstream-refs.env"
# An unset run_verification means "verify": only an explicit false opts a target out.
assert_contains 't.get("run_verification", True) is False' "$PLAN" "commaviewd CI should run verification unless a target opts out"

# The workflow.
assert_contains "comma4/install.sh" "$WORKFLOW" "commaviewd CI should run when the legacy install shim changes"
assert_contains "ci/plan-targets.py" "$WORKFLOW" "commaviewd CI should plan its targets with ci/plan-targets.py"
assert_contains 'target: ${{ fromJSON(needs.plan.outputs.targets) }}' "$WORKFLOW" "commaviewd CI matrix should come from the plan"
assert_contains '--platform "${{ matrix.target.ui_platform }}"' "$WORKFLOW" "commaviewd CI should pass explicit MICI/TICI/TIZI platform"
assert_contains 'ref: ${{ matrix.target.sha }}' "$WORKFLOW" "commaviewd CI upstream checkout should be pinned to the planned SHA"
assert_contains 'if: matrix.target.run_verification' "$WORKFLOW" "commaviewd CI should run verification for the targets the plan picks"
if grep -Fq 'run_verification != false }}' "$WORKFLOW"; then
  echo "FAIL: 'run_verification != false' treats an unset key as false and skips verification" >&2
  exit 1
fi

printf 'PASS: CI workflow contract validates pinned upstream checkout\n'
