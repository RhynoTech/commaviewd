#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
WORKFLOW="$REPO_ROOT/.github/workflows/commaviewd-release.yml"

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
assert_contains "Onroad UI export transformer apply/verify" "$WORKFLOW" "release workflow should apply/verify transformer before packaging"
assert_contains "apply_onroad_ui_export_patch.sh" "$WORKFLOW" "release workflow should apply transformer"
assert_contains "verify_onroad_ui_export_patch.sh --json" "$WORKFLOW" "release workflow should verify transformer"
assert_contains "onroad-ui-export-status.json" "$WORKFLOW" "release workflow should preserve transformer status manifest"
assert_contains "git -C \"\${{ github.workspace }}/openpilot-src\" reset --hard -q HEAD" "$WORKFLOW" "release workflow should reset upstream tree after transformer check"
assert_contains "git -C \"\${{ github.workspace }}/openpilot-src\" clean -fdq" "$WORKFLOW" "release workflow should clean upstream tree after transformer check"

release_gate_line="$(grep -n "Onroad UI export transformer apply/verify" "$WORKFLOW" | cut -d: -f1 | head -1)"
verification_line="$(grep -n "Run release verification pipeline" "$WORKFLOW" | cut -d: -f1 | head -1)"
build_line="$(grep -n "Build release bundle" "$WORKFLOW" | cut -d: -f1 | head -1)"
if [[ -z "$release_gate_line" || -z "$verification_line" || -z "$build_line" ]]; then
  echo "FAIL: unable to locate release gate, verification, or build step" >&2
  exit 1
fi
if (( release_gate_line >= verification_line || verification_line >= build_line )); then
  echo "FAIL: release transformer gate must run before verification and build" >&2
  exit 1
fi

printf 'PASS: release workflow contract validates transformer gate before packaging\n'
