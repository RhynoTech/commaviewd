#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/path/to/openpilot-src commaviewd/scripts/reproducible-build.sh [--manifest <path>]
Builds commaviewd twice and verifies host + aarch64 digests are identical.
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
DEFAULT_OP_ROOT="$REPO_ROOT/../openpilot-src"
if [[ -d "$DEFAULT_OP_ROOT" ]]; then
  OP_ROOT="${OP_ROOT:-$DEFAULT_OP_ROOT}"
else
  OP_ROOT="${OP_ROOT:-$HOME/openpilot-src}"
fi
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
MANIFEST="$DIST_DIR/reproducible-build-manifest.json"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest) MANIFEST="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

BUILD_SCRIPT="$ROOT/scripts/build-ubuntu.sh"
HOST_BIN="$DIST_DIR/commaviewd-host"
ARM_BIN="$DIST_DIR/commaviewd-aarch64"

run_build() {
  SOURCE_DATE_EPOCH=1704067200 OP_ROOT="$OP_ROOT" DIST_DIR="$DIST_DIR" "$BUILD_SCRIPT" >/dev/null
}

run_build
sha_host_1="$(sha256sum "$HOST_BIN" | awk '{print $1}')"
sha_arm_1="$(sha256sum "$ARM_BIN" | awk '{print $1}')"

run_build
sha_host_2="$(sha256sum "$HOST_BIN" | awk '{print $1}')"
sha_arm_2="$(sha256sum "$ARM_BIN" | awk '{print $1}')"

if [[ "$sha_host_1" != "$sha_host_2" || "$sha_arm_1" != "$sha_arm_2" ]]; then
  echo "FAIL: reproducibility mismatch" >&2
  echo "host: $sha_host_1 vs $sha_host_2" >&2
  echo "arm : $sha_arm_1 vs $sha_arm_2" >&2
  exit 1
fi

mkdir -p "$(dirname "$MANIFEST")"
cat > "$MANIFEST" <<JSON
{
  "sourceDateEpoch": 1704067200,
  "opRoot": "${OP_ROOT}",
  "artifacts": {
    "commaviewd-host": "${sha_host_1}",
    "commaviewd-aarch64": "${sha_arm_1}"
  }
}
JSON

echo "PASS: reproducible build verified"
echo "host sha256: $sha_host_1"
echo "arm  sha256: $sha_arm_1"
echo "manifest: $MANIFEST"
