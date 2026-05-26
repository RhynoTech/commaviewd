#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
WORKFLOW="$REPO_ROOT/.github/workflows/commaviewd-device-test.yml"

assert_file() {
  [[ -f "$1" ]] || { echo "FAIL: missing $1" >&2; exit 1; }
}

assert_contains() {
  local needle="$1"
  local file="$2"
  local message="$3"
  grep -Fq -- "$needle" "$file" || { echo "FAIL: $message" >&2; exit 1; }
}

assert_not_contains() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if grep -Fq -- "$needle" "$file"; then
    echo "FAIL: $message" >&2
    exit 1
  fi
}

assert_file "$WORKFLOW"

assert_contains "workflow_dispatch:" "$WORKFLOW" "device-test workflow should be manual dispatch only"
assert_not_contains "push:" "$WORKFLOW" "device-test workflow must not run on push"
assert_not_contains "pull_request:" "$WORKFLOW" "device-test workflow must not run on pull request"
assert_contains "name: openpilot-release-mici-staging" "$WORKFLOW" "device-test matrix should include openpilot release-mici-staging"
assert_contains "upstream_repo: commaai/openpilot" "$WORKFLOW" "device-test matrix should include commaai/openpilot"
assert_contains "upstream_ref: release-mici-staging" "$WORKFLOW" "device-test matrix should include openpilot release-mici-staging ref"
assert_contains "name: sunnypilot-staging" "$WORKFLOW" "device-test matrix should include sunnypilot staging"
assert_contains "upstream_repo: sunnypilot/sunnypilot" "$WORKFLOW" "device-test matrix should include sunnypilot/sunnypilot"
assert_contains "upstream_ref: staging" "$WORKFLOW" "device-test matrix should include sunnypilot staging ref"
assert_contains "apply_onroad_ui_export_patch.sh" "$WORKFLOW" "device-test workflow should apply the transformer patch"
assert_contains "verify_onroad_ui_export_patch.sh" "$WORKFLOW" "device-test workflow should verify the transformer patch"
assert_contains "commaviewd/scripts/run-verification.sh" "$WORKFLOW" "device-test workflow should run full verification"
assert_contains "tools/release/comma4-build-bundle.sh" "$WORKFLOW" "device-test workflow should build release-shaped bundles"
assert_contains 'ref: ${{ steps.upstream.outputs.sha }}' "$WORKFLOW" "device-test upstream checkout should be pinned to resolved SHA"
assert_contains "actions/upload-artifact" "$WORKFLOW" "device-test workflow should upload artifacts"
assert_contains "Upload device-test diagnostics on failure" "$WORKFLOW" "device-test workflow should upload failure diagnostics"
assert_contains "if-no-files-found: warn" "$WORKFLOW" "device-test failure diagnostics should be best-effort"
assert_contains "retention-days: 7" "$WORKFLOW" "device-test artifacts should be short-retention"
assert_contains "device-test-manifest.json" "$WORKFLOW" "device-test workflow should write a manifest"
assert_contains "sha256sum" "$WORKFLOW" "device-test workflow should print checksum information"
assert_not_contains "gh release" "$WORKFLOW" "device-test workflow must not create or edit GitHub releases"
assert_not_contains "update-firebase-current-release" "$WORKFLOW" "device-test workflow must not update current release pointers"

printf 'PASS: device-test workflow contract validates non-release RC artifacts\n'
