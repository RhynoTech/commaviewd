#!/usr/bin/env bash
set -euo pipefail

TAILSCALE_ROOT="${COMMAVIEWD_TAILSCALE_INSTALL_ROOT:-/data/commaview/tailscale}"
BIN_DIR="$TAILSCALE_ROOT/bin"
TMP_DIR="${COMMAVIEWD_TAILSCALE_TMP_DIR:-/tmp/commaview-tailscale-runtime}"
TAILSCALE_VERSION="${COMMAVIEWD_TAILSCALE_VERSION:-1.80.3}"

log() {
  echo "[tailscale-runtime] $*"
}

detect_arch() {
  local arch
  arch="$(uname -m)"
  case "$arch" in
    aarch64|arm64) echo "arm64" ;;
    x86_64|amd64) echo "amd64" ;;
    *)
      echo "Unsupported architecture: $arch" >&2
      return 1
      ;;
  esac
}

install_from_system_path() {
  local ts_bin tsd_bin
  ts_bin="$(command -v tailscale 2>/dev/null || true)"
  tsd_bin="$(command -v tailscaled 2>/dev/null || true)"

  if [ -n "$ts_bin" ] && [ -n "$tsd_bin" ]; then
    cp "$ts_bin" "$BIN_DIR/tailscale"
    cp "$tsd_bin" "$BIN_DIR/tailscaled"
    chmod +x "$BIN_DIR/tailscale" "$BIN_DIR/tailscaled"
    log "Copied tailscale binaries from system PATH"
    return 0
  fi

  return 1
}

install_from_release_tarball() {
  local arch tarball url extracted_dir
  arch="$(detect_arch)"
  mkdir -p "$TMP_DIR"
  tarball="$TMP_DIR/tailscale_${TAILSCALE_VERSION}_${arch}.tgz"
  url="https://pkgs.tailscale.com/stable/tailscale_${TAILSCALE_VERSION}_${arch}.tgz"

  log "Downloading $url"
  curl -fsSL "$url" -o "$tarball"

  extracted_dir="$TMP_DIR/extract"
  rm -rf "$extracted_dir"
  mkdir -p "$extracted_dir"
  tar -xzf "$tarball" -C "$extracted_dir"

  local ts_bin tsd_bin
  ts_bin="$(find "$extracted_dir" -type f -name tailscale | head -1)"
  tsd_bin="$(find "$extracted_dir" -type f -name tailscaled | head -1)"

  if [ -z "$ts_bin" ] || [ -z "$tsd_bin" ]; then
    echo "Failed to locate tailscale binaries in downloaded archive" >&2
    return 1
  fi

  cp "$ts_bin" "$BIN_DIR/tailscale"
  cp "$tsd_bin" "$BIN_DIR/tailscaled"
  chmod +x "$BIN_DIR/tailscale" "$BIN_DIR/tailscaled"
  log "Installed tailscale binaries from release tarball"
}

main() {
  mkdir -p "$BIN_DIR"

  if [ -x "$BIN_DIR/tailscale" ] && [ -x "$BIN_DIR/tailscaled" ]; then
    log "Runtime already installed at $BIN_DIR"
    exit 0
  fi

  if ! install_from_system_path; then
    install_from_release_tarball
  fi

  if [ ! -x "$BIN_DIR/tailscale" ] || [ ! -x "$BIN_DIR/tailscaled" ]; then
    echo "Failed to install tailscale runtime" >&2
    exit 1
  fi

  log "Runtime ready"
}

main "$@"
