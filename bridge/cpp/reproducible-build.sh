#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/home/pear/openpilot-src bridge/cpp/reproducible-build.sh [--manifest <path>]

Builds the bridge twice and verifies host + aarch64 artifact SHA256 digests are identical.
Writes a JSON manifest with toolchain versions and checksums.
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "$0")" && pwd)"
OP_ROOT="${OP_ROOT:-/home/pear/openpilot-src}"
MANIFEST="$ROOT/reproducible-build-manifest.json"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest)
      MANIFEST="$2"
      shift 2
      ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2
      ;;
  esac
done

BUILD_SCRIPT="$ROOT/build-ubuntu.sh"
HOST_BIN="$ROOT/commaview-bridge-host"
ARM_BIN="$ROOT/commaview-bridge-aarch64"

run_build() {
  SOURCE_DATE_EPOCH=1704067200 OP_ROOT="$OP_ROOT" "$BUILD_SCRIPT" >/dev/null
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

host_cxx="$(clang++ --version | head -1)"
arm_cxx="$(aarch64-linux-gnu-g++ --version | head -1)"
capnpc_ver="$(capnpc --version 2>&1 | head -1)"

cat > "$MANIFEST" <<JSON
{
  "sourceDateEpoch": 1704067200,
  "opRoot": "${OP_ROOT}",
  "tools": {
    "hostCxx": "${host_cxx}",
    "armCxx": "${arm_cxx}",
    "capnpc": "${capnpc_ver}"
  },
  "artifacts": {
    "commaview-bridge-host": "${sha_host_1}",
    "commaview-bridge-aarch64": "${sha_arm_1}"
  }
}
JSON

echo "PASS: reproducible build verified"
echo "host sha256: $sha_host_1"
echo "arm  sha256: $sha_arm_1"
echo "manifest: $MANIFEST"
