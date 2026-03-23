#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
CI="$REPO_ROOT/.github/workflows/commaviewd-ci.yml"
CANARY_OPENPILOT="$REPO_ROOT/.github/workflows/commaviewd-canary-openpilot.yml"
CANARY_SUNNYPILOT="$REPO_ROOT/.github/workflows/commaviewd-canary-sunnypilot.yml"

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

assert_contains_fixed() {
  local needle="$1"
  local file="$2"
  local message="$3"
  grep -Fq "$needle" "$file" || fail "$message"
}

assert_not_contains_fixed() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if grep -Fq "$needle" "$file"; then
    fail "$message"
  fi
}

for file in "$CI" "$CANARY_OPENPILOT" "$CANARY_SUNNYPILOT"; do
  [[ -f "$file" ]] || fail "missing workflow $file"
  assert_not_contains_fixed "android-schema" "$file" "$file should not reference android-schema after direct v2 cutover"
  assert_not_contains_fixed "check-android-schema-drift" "$file" "$file should not reference schema drift scripts after direct v2 cutover"
  assert_not_contains_fixed "dist/android-schema-drift.json" "$file" "$file should not upload schema drift artifacts after direct v2 cutover"
  assert_not_contains_fixed "apply_hud_lite_patch.sh" "$file" "$file should stop referencing stale HUD-lite helpers"
  assert_not_contains_fixed "verify_hud_lite_patch.sh" "$file" "$file should stop referencing stale HUD-lite helpers"
  assert_not_contains_fixed "hud-lite-status.json" "$file" "$file should stop surfacing stale HUD-lite artifacts"
  assert_contains_fixed "apply_onroad_ui_export_patch.sh" "$file" "$file should validate direct v2 patch applicability"
  assert_contains_fixed "verify_onroad_ui_export_patch.sh" "$file" "$file should verify direct v2 patch applicability"
  assert_contains_fixed "onroad-ui-export-status.json" "$file" "$file should surface direct v2 status artifacts"
done

echo "PASS: workflows are aligned to direct v2 validation"
