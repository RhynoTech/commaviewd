#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
WORKFLOW="$REPO_ROOT/.github/workflows/commaviewd-release.yml"
BUILD_BUNDLE="$REPO_ROOT/tools/release/comma-build-bundle.sh"
FIREBASE_UPDATE="$REPO_ROOT/scripts/update-firebase-current-release.mjs"

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
assert_file "$BUILD_BUNDLE"
assert_file "$FIREBASE_UPDATE"
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
assert_contains "compareRuntimeTags" "$FIREBASE_UPDATE" "Firebase updater should compare runtime tags before promotion"
assert_contains "Refusing to promote older runtime tag" "$FIREBASE_UPDATE" "Firebase updater should fail closed on runtime downgrades"
assert_contains "allow-runtime-downgrade" "$FIREBASE_UPDATE" "Firebase updater should require an explicit downgrade override if ever needed"
assert_contains 'args["allow-runtime-downgrade"] !== "true"' "$FIREBASE_UPDATE" "runtime downgrade override should be explicit"
assert_contains "Checkout release control-plane scripts" "$WORKFLOW" "promotion should checkout guarded updater from master after release-tag checkout"
assert_contains "path: release-control" "$WORKFLOW" "promotion should keep master control scripts separate from release tag workspace"
assert_contains "node release-control/scripts/update-firebase-current-release.mjs" "$WORKFLOW" "promotion should run guarded updater from master, not old release tag"
assert_contains "ref: master" "$WORKFLOW" "promotion control checkout should pin to master"
assert_contains "PROVENANCE_ASSETS=(" "$WORKFLOW" "release workflow should define provenance assets for upload"
assert_contains 'REQUIRED_ASSETS=("$ASSET_TGZ" "$ASSET_SHA" "${PROVENANCE_ASSETS[@]}")' "$WORKFLOW" "release workflow should validate bundle, checksum, and provenance assets"
assert_contains 'cd "$OUT_DIR"' "$BUILD_BUNDLE" "bundle script should enter release directory before writing checksum"
assert_contains 'sha256sum "${NAME}.tar.gz" > "${NAME}.tar.gz.sha256"' "$BUILD_BUNDLE" "bundle script should write checksum with portable asset basename"
for staged_asset in \
  'install -m 755 "${ROOT}/comma/install.sh" "${STAGE_DIR}/install.sh"' \
  'install -m 755 "${ROOT}/comma/scripts/apply_onroad_ui_export_patch.sh" "${STAGE_DIR}/scripts/apply_onroad_ui_export_patch.sh"' \
  'install -m 755 "${ROOT}/comma/scripts/revert_onroad_ui_export_patch.sh" "${STAGE_DIR}/scripts/revert_onroad_ui_export_patch.sh"' \
  'install -m 755 "${ROOT}/comma/scripts/verify_onroad_ui_export_patch.sh" "${STAGE_DIR}/scripts/verify_onroad_ui_export_patch.sh"' \
  'install -m 755 "${ROOT}/comma/scripts/transform_onroad_ui_export.py" "${STAGE_DIR}/scripts/transform_onroad_ui_export.py"' \
  'install -m 755 "${ROOT}/comma/scripts/smoke_onroad_ui_export_helper.py" "${STAGE_DIR}/scripts/smoke_onroad_ui_export_helper.py"' \
  'install -m 644 "${ROOT}/comma/src/commaview_export.openpilot.py" "${STAGE_DIR}/src/commaview_export.openpilot.py"' \
  'install -m 644 "${ROOT}/comma/src/commaview_export.sunnypilot.py" "${STAGE_DIR}/src/commaview_export.sunnypilot.py"'; do
  assert_contains "$staged_asset" "$BUILD_BUNDLE" "bundle script should stage UI export asset: $staged_asset"
done
for asset in \
  "dist/reproducible-build-manifest.json" \
  "dist/upstream-interface-manifest.json" \
  "dist/binary-contract.json" \
  "dist/release-smoke-manifest.json" \
  "dist/onroad-ui-export-status.json"; do
  assert_contains "$asset" "$WORKFLOW" "release workflow should publish provenance asset $asset"
done
assert_contains "Missing release asset: \$asset" "$WORKFLOW" "release workflow should fail clearly when any release asset is missing"
assert_contains '(cd "release/${TAG}" && sha256sum -c "commaview-comma-${TAG}.tar.gz.sha256")' "$WORKFLOW" "release workflow should validate portable checksum file from inside release directory"
assert_contains 'for manifest in "${PROVENANCE_ASSETS[@]}"; do' "$WORKFLOW" "release workflow should validate every provenance JSON manifest"
assert_contains 'python3 -m json.tool "$manifest" >/dev/null' "$WORKFLOW" "release workflow should parse provenance manifests as JSON before publishing"
assert_contains 'gh release upload "$TAG" "$ASSET_TGZ" "$ASSET_SHA" "${PROVENANCE_ASSETS[@]}"' "$WORKFLOW" "release workflow should upload bundle, checksum, and provenance manifests together"

release_asset_validation_line="$(grep -n "REQUIRED_ASSETS=(" "$WORKFLOW" | cut -d: -f1 | head -1)"
checksum_validation_line="$(grep -n 'sha256sum -c "commaview-comma-${TAG}.tar.gz.sha256"' "$WORKFLOW" | cut -d: -f1 | head -1)"
json_validation_line="$(grep -n 'python3 -m json.tool "$manifest" >/dev/null' "$WORKFLOW" | cut -d: -f1 | head -1)"
release_create_line="$(grep -n "gh release create" "$WORKFLOW" | cut -d: -f1 | head -1)"
release_edit_line="$(grep -n "gh release edit" "$WORKFLOW" | cut -d: -f1 | head -1)"
if [[ -z "$release_asset_validation_line" || -z "$checksum_validation_line" || -z "$json_validation_line" || -z "$release_create_line" || -z "$release_edit_line" ]]; then
  echo "FAIL: unable to locate release asset checksum/JSON validation or release create/edit steps" >&2
  exit 1
fi
for validation_line in "$release_asset_validation_line" "$checksum_validation_line" "$json_validation_line"; do
  if (( validation_line >= release_create_line || validation_line >= release_edit_line )); then
    echo "FAIL: release asset checksum/JSON validation must run before GitHub release create/edit" >&2
    exit 1
  fi
done

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
