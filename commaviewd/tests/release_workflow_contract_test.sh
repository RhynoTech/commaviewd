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
assert_contains "Release \$TAG already exists; overwrite requires manual dispatch with allow_overwrite=true" "$WORKFLOW" "existing GitHub releases should not be overwritten unless manual dispatch explicitly allows it"
assert_contains "promote_current:" "$WORKFLOW" "release workflow should expose explicit Firebase runtime promotion input"
assert_contains "Promote this runtime tag to Firebase current-release after publishing assets" "$WORKFLOW" "Firebase runtime promotion input should explain the explicit promotion action"
assert_contains "if: github.event_name == 'workflow_dispatch' && inputs.promote_current == true" "$WORKFLOW" "Firebase current-release update should require explicit manual promotion"
assert_contains "Promote Firebase current runtime release" "$WORKFLOW" "Firebase current-release step should be named as an explicit promotion"
assert_contains "PROVENANCE_ASSETS=(" "$WORKFLOW" "release workflow should define provenance assets for upload"
assert_contains 'REQUIRED_ASSETS=("$ASSET_TGZ" "$ASSET_SHA" "${PROVENANCE_ASSETS[@]}")' "$WORKFLOW" "release workflow should validate bundle, checksum, and provenance assets"
for asset in \
  "dist/reproducible-build-manifest.json" \
  "dist/upstream-interface-manifest.json" \
  "dist/binary-contract.json" \
  "dist/release-smoke-manifest.json" \
  "dist/onroad-ui-export-status.json"; do
  assert_contains "$asset" "$WORKFLOW" "release workflow should publish provenance asset $asset"
done
assert_contains "Missing release asset: \$asset" "$WORKFLOW" "release workflow should fail clearly when any release asset is missing"
assert_contains 'gh release upload "$TAG" "$ASSET_TGZ" "$ASSET_SHA" "${PROVENANCE_ASSETS[@]}"' "$WORKFLOW" "release workflow should upload bundle, checksum, and provenance manifests together"

release_asset_validation_line="$(grep -n "REQUIRED_ASSETS=(" "$WORKFLOW" | cut -d: -f1 | head -1)"
release_create_line="$(grep -n "gh release create" "$WORKFLOW" | cut -d: -f1 | head -1)"
release_edit_line="$(grep -n "gh release edit" "$WORKFLOW" | cut -d: -f1 | head -1)"
if [[ -z "$release_asset_validation_line" || -z "$release_create_line" || -z "$release_edit_line" ]]; then
  echo "FAIL: unable to locate release asset validation or release create/edit steps" >&2
  exit 1
fi
if (( release_asset_validation_line >= release_create_line || release_asset_validation_line >= release_edit_line )); then
  echo "FAIL: release asset validation must run before GitHub release create/edit" >&2
  exit 1
fi

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
