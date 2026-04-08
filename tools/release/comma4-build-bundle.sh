#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: tools/release/comma4-build-bundle.sh [--skip-build] [<tag>]
Builds/stages CommaView comma4 bundle and outputs:
  release/<tag>/commaview-comma4-<tag>.tar.gz
  release/<tag>/commaview-comma4-<tag>.tar.gz.sha256
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION_ENV="${ROOT}/comma4/version.env"
if [[ ! -f "$VERSION_ENV" ]]; then
  echo "ERROR: missing required version source: $VERSION_ENV" >&2
  exit 1
fi
# shellcheck disable=SC1090
. "$VERSION_ENV"
if [[ -z "${RELEASE_TAG:-}" ]]; then
  echo "ERROR: version.env must define RELEASE_TAG" >&2
  exit 1
fi

SKIP_BUILD=0
TAG_OVERRIDE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --skip-build) SKIP_BUILD=1; shift ;;
    --help|-h)
      "$0" --help
      exit 0
      ;;
    *)
      if [[ -n "$TAG_OVERRIDE" ]]; then
        echo "ERROR: multiple tag arguments provided" >&2
        exit 2
      fi
      TAG_OVERRIDE="$1"
      shift
      ;;
  esac
done

TAG="${TAG_OVERRIDE:-$RELEASE_TAG}"
NAME="commaview-comma4-${TAG}"
OUT_DIR="${ROOT}/release/${TAG}"
STAGE_DIR="${OUT_DIR}/${NAME}"
ASSET_TGZ="${OUT_DIR}/${NAME}.tar.gz"
ASSET_SHA="${ASSET_TGZ}.sha256"
DIST_DIR="${DIST_DIR:-${ROOT}/dist}"

required_stage_files=(
  "commaviewd"
  "upgrade.sh"
  "start.sh"
  "stop.sh"
  "uninstall.sh"
)

validate_stage_contents() {
  local missing=0
  for rel in "${required_stage_files[@]}"; do
    if [[ ! -f "$STAGE_DIR/$rel" ]]; then
      echo "ERROR: staged bundle missing $rel" >&2
      missing=1
    fi
  done

  local capnp_count kj_count
  capnp_count=$(find "$STAGE_DIR/lib" -maxdepth 1 -type f -name 'libcapnp-*.so' | wc -l | tr -d ' ')
  kj_count=$(find "$STAGE_DIR/lib" -maxdepth 1 -type f -name 'libkj-*.so' | wc -l | tr -d ' ')

  if [[ "$capnp_count" -eq 0 ]]; then
    echo "ERROR: staged bundle missing libcapnp-*.so" >&2
    missing=1
  fi
  if [[ "$kj_count" -eq 0 ]]; then
    echo "ERROR: staged bundle missing libkj-*.so" >&2
    missing=1
  fi

  [[ "$missing" -eq 0 ]] || exit 1
}

mkdir -p "$OUT_DIR"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/lib"

if [[ "$SKIP_BUILD" -eq 1 ]]; then
  echo "[1/3] Skipping build (using existing dist artifacts)..."
else
  echo "[1/3] Building commaviewd artifacts..."
  DIST_DIR="$DIST_DIR" "${ROOT}/commaviewd/scripts/build-ubuntu.sh"
fi

echo "[2/3] Staging bundle files..."
install -m 755 "${DIST_DIR}/commaviewd-aarch64" "${STAGE_DIR}/commaviewd"

shopt -s nullglob
capnp_libs=("${DIST_DIR}/lib"/libcapnp-*.so)
kj_libs=("${DIST_DIR}/lib"/libkj-*.so)
shopt -u nullglob

if [[ ${#capnp_libs[@]} -eq 0 ]]; then
  echo "ERROR: dist missing libcapnp-*.so under ${DIST_DIR}/lib" >&2
  exit 1
fi
if [[ ${#kj_libs[@]} -eq 0 ]]; then
  echo "ERROR: dist missing libkj-*.so under ${DIST_DIR}/lib" >&2
  exit 1
fi

for src in "${capnp_libs[@]}" "${kj_libs[@]}"; do
  install -m 755 "$src" "${STAGE_DIR}/lib/$(basename "$src")"
done

install -m 755 "${ROOT}/comma4/upgrade.sh" "${STAGE_DIR}/upgrade.sh"
install -m 755 "${ROOT}/comma4/start.sh" "${STAGE_DIR}/start.sh"
install -m 755 "${ROOT}/comma4/stop.sh" "${STAGE_DIR}/stop.sh"
install -m 755 "${ROOT}/comma4/uninstall.sh" "${STAGE_DIR}/uninstall.sh"

cat > "${STAGE_DIR}/VERSION" <<VER
${TAG}
VER

validate_stage_contents

echo "[3/3] Packing release asset..."
(
  cd "$OUT_DIR"
  tar -czf "${NAME}.tar.gz" "${NAME}"
)
sha256sum "$ASSET_TGZ" > "$ASSET_SHA"

ls -lh "$ASSET_TGZ" "$ASSET_SHA"
cat "$ASSET_SHA"
echo "Bundle ready: $ASSET_TGZ"
