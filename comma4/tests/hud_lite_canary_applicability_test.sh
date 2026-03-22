#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SYNC_SCRIPT="$REPO_ROOT/scripts/sync-canary-upstream.sh"
APPLY_SCRIPT="$REPO_ROOT/comma4/scripts/apply_hud_lite_patch.sh"
INSTALL_DIR="$REPO_ROOT/comma4"
BASE_DIR="${HUD_LITE_CANARY_TEST_BASE:-$(mktemp -d /tmp/commaviewd-hud-lite-canary.XXXXXX)}"
KEEP_BASE="${HUD_LITE_CANARY_TEST_KEEP:-0}"

cleanup() {
  if [ "$KEEP_BASE" = "1" ]; then
    echo "INFO: keeping $BASE_DIR"
    return
  fi
  rm -rf "$BASE_DIR"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

run_ref() {
  local upstream="$1"
  local ref="$2"
  local root="$BASE_DIR/${upstream}-${ref}"
  local checkout="$root/openpilot-src"
  local output

  echo "=== ${upstream} ${ref} ==="
  "$SYNC_SCRIPT" "$upstream" "$ref" "$root" >/dev/null
  git -C "$checkout" rev-parse HEAD >/dev/null 2>&1 || fail "sync produced invalid checkout for ${upstream} ${ref}"
  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq

  output="$(COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" COMMAVIEWD_OP_ROOT="$checkout" "$APPLY_SCRIPT" 2>&1)" || {
    printf '%s\n' "$output" >&2
    fail "HUD-lite patch apply failed for ${upstream} ${ref}"
  }

  printf '%s\n' "$output"
  grep -Fq "\"healthy\":false" <<<"$output" || fail "expected healthy=false for marker-only verification"
grep -Fq "\"patchVerified\":true" <<<"$output" || fail "expected patchVerified=true"
grep -Fq "\"statusScope\":\"patch-installation\"" <<<"$output" || fail "expected statusScope=patch-installation"
  grep -Fq "\"servicePresent\":true" <<<"$output" || fail "expected servicePresent=true"
  grep -Fq "\"structPresent\":true" <<<"$output" || fail "expected structPresent=true"
  grep -Fq "\"publisherPresent\":true" <<<"$output" || fail "expected publisherPresent=true"
  grep -Fq "\"logEventPresent\":true" <<<"$output" || fail "expected logEventPresent=true"

  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq
}

run_ref openpilot release-mici-staging
run_ref openpilot nightly
run_ref sunnypilot staging
run_ref sunnypilot dev

echo "PASS: HUD-lite patch applies and verifies on real canary refs"
