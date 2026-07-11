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

assert_file "$WORKFLOW"
assert_contains "name: openpilot-release-mici" "$WORKFLOW" "commaviewd CI should validate openpilot release-mici"
assert_contains "upstream_ref: release-mici" "$WORKFLOW" "commaviewd CI should include MICI release refs"
assert_contains "name: openpilot-release-tici" "$WORKFLOW" "commaviewd CI should validate openpilot release-tici"
assert_contains "upstream_ref: release-tici" "$WORKFLOW" "commaviewd CI should include openpilot TICI release ref"
assert_contains "upstream_repo: commaai/openpilot" "$WORKFLOW" "commaviewd CI should include commaai/openpilot"
assert_contains "name: sunnypilot-release-mici" "$WORKFLOW" "commaviewd CI should validate sunnypilot release-mici"
assert_contains "name: sunnypilot-release-tizi" "$WORKFLOW" "commaviewd CI should validate sunnypilot release-tizi"
assert_contains "upstream_ref: release-tizi" "$WORKFLOW" "commaviewd CI should include sunnypilot TIZI release ref"
assert_contains "upstream_repo: sunnypilot/sunnypilot" "$WORKFLOW" "commaviewd CI should include sunnypilot/sunnypilot"
assert_contains '--platform "${{ matrix.target.ui_platform }}"' "$WORKFLOW" "commaviewd CI should pass explicit MICI/TICI/TIZI platform"
assert_contains "Resolve upstream SHA" "$WORKFLOW" "commaviewd CI should resolve upstream SHA before cache/checkout"
assert_contains "id: upstream" "$WORKFLOW" "commaviewd CI resolved upstream SHA step should use id upstream"
assert_contains 'ref: ${{ steps.upstream.outputs.sha }}' "$WORKFLOW" "commaviewd CI upstream checkout should be pinned to resolved SHA"

printf 'PASS: CI workflow contract validates pinned upstream checkout\n'
