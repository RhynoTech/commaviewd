#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/home/pear/openpilot-src commaviewd/scripts/run-verification.sh
Runs full verification pipeline:
  1) upstream interface guard
  2) reproducible build
  3) binary contract check
  4) unit tests
  5) release bundle smoke package (no rebuild)
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
OP_ROOT="${OP_ROOT:-/home/pear/openpilot-src}"
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
RELEASE_SMOKE_TAG="${RELEASE_SMOKE_TAG:-ci-smoke}"
RELEASE_SMOKE_MANIFEST="$DIST_DIR/release-smoke-manifest.json"

OP_ROOT="$OP_ROOT" DIST_DIR="$DIST_DIR" "$ROOT/scripts/upstream-interface-guard.sh"
OP_ROOT="$OP_ROOT" DIST_DIR="$DIST_DIR" "$ROOT/scripts/reproducible-build.sh"
DIST_DIR="$DIST_DIR" "$ROOT/scripts/binary-contract-check.sh"
OP_ROOT="$OP_ROOT" DIST_DIR="$DIST_DIR" "$ROOT/scripts/run-unit-tests.sh"

DIST_DIR="$DIST_DIR" "$REPO_ROOT/tools/release/comma4-build-bundle.sh" --skip-build "$RELEASE_SMOKE_TAG"

release_asset="$REPO_ROOT/release/$RELEASE_SMOKE_TAG/commaview-comma4-$RELEASE_SMOKE_TAG.tar.gz"
release_sha="$release_asset.sha256"
[[ -f "$release_asset" ]] || { echo "FAIL: missing release smoke asset $release_asset" >&2; exit 1; }
[[ -f "$release_sha" ]] || { echo "FAIL: missing release smoke checksum $release_sha" >&2; exit 1; }

mkdir -p "$(dirname "$RELEASE_SMOKE_MANIFEST")"
cat > "$RELEASE_SMOKE_MANIFEST" <<JSON
{
  "tag": "${RELEASE_SMOKE_TAG}",
  "asset": "${release_asset}",
  "sha256File": "${release_sha}"
}
JSON

echo "PASS: commaviewd verification pipeline complete"
echo "release smoke manifest: $RELEASE_SMOKE_MANIFEST"
