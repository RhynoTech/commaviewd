#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TAG="${1:-v0.1.4-alpha}"
NAME="commaview-comma4-${TAG}"
OUT_DIR="${ROOT}/release/${TAG}"
STAGE_DIR="${OUT_DIR}/${NAME}"
ASSET_TGZ="${OUT_DIR}/${NAME}.tar.gz"
ASSET_SHA="${ASSET_TGZ}.sha256"
DIST_DIR="${ROOT}/dist"

required_stage_files=(
  "commaviewd"
  "lib/libcapnp-0.8.0.so"
  "lib/libkj-0.8.0.so"
  "upgrade.sh"
  "start.sh"
  "stop.sh"
  "uninstall.sh"
  "tailscale/install_tailscale_runtime.sh"
)

validate_stage_contents() {
  local missing=0
  for rel in "${required_stage_files[@]}"; do
    if [[ ! -f "$STAGE_DIR/$rel" ]]; then
      echo "ERROR: staged bundle missing $rel" >&2
      missing=1
    fi
  done
  [[ "$missing" -eq 0 ]] || exit 1
}

mkdir -p "$OUT_DIR"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/lib" "$STAGE_DIR/tailscale"

echo "[1/3] Building commaviewd artifacts..."
DIST_DIR="$DIST_DIR" "${ROOT}/commaviewd/scripts/build-ubuntu.sh"

echo "[2/3] Staging bundle files..."
install -m 755 "${DIST_DIR}/commaviewd-aarch64" "${STAGE_DIR}/commaviewd"
install -m 755 "${DIST_DIR}/lib/libcapnp-0.8.0.so" "${STAGE_DIR}/lib/libcapnp-0.8.0.so"
install -m 755 "${DIST_DIR}/lib/libkj-0.8.0.so" "${STAGE_DIR}/lib/libkj-0.8.0.so"

install -m 755 "${ROOT}/comma4/upgrade.sh" "${STAGE_DIR}/upgrade.sh"
install -m 755 "${ROOT}/comma4/start.sh" "${STAGE_DIR}/start.sh"
install -m 755 "${ROOT}/comma4/stop.sh" "${STAGE_DIR}/stop.sh"
install -m 755 "${ROOT}/comma4/uninstall.sh" "${STAGE_DIR}/uninstall.sh"
install -m 755 "${ROOT}/comma4/install_tailscale_runtime.sh" "${STAGE_DIR}/tailscale/install_tailscale_runtime.sh"

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
