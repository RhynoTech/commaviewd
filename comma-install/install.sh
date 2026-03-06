#!/usr/bin/env bash
# CommaView installer for comma 4 (AGNOS/sunnypilot)
# Installs prebuilt C++ bridge bundle from pinned GitHub release.
set -euo pipefail

VERSION="0.1.2-alpha"
RELEASE_TAG="v0.1.2-alpha"
GITHUB_REPO="${COMMAVIEW_RELEASE_REPO:-RhynoTech/CommaView}"
ASSET_NAME="${COMMAVIEW_ASSET_NAME:-commaview-comma4-${RELEASE_TAG}.tar.gz}"
ASSET_SHA_NAME="${ASSET_NAME}.sha256"
BASE_URL="${COMMAVIEW_BASE_URL:-https://github.com/${GITHUB_REPO}/releases/download/${RELEASE_TAG}}"

INSTALL_DIR="/data/commaview"
CONTINUE_SH="/data/continue.sh"
MARKER="# commaview-hook"

ENABLE_TAILSCALE=0
TAILSCALE_AUTHKEY="${COMMAVIEW_TAILSCALE_AUTHKEY:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"

usage() {
  cat <<USAGE
CommaView installer ${VERSION}

Usage:
  install.sh [--enable-tailscale] [--tailscale-auth-key <key>] [--help]

Options:
  --enable-tailscale             Enable optional Tailscale Guardian setup.
  --tailscale-auth-key <key>     One-time auth key used during initial join.
                                 You can also set COMMAVIEW_TAILSCALE_AUTHKEY.
  -h, --help                     Show this help and exit.

Default behavior:
  If --enable-tailscale is not set, install path is unchanged.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --enable-tailscale)
      ENABLE_TAILSCALE=1
      shift
      ;;
    --tailscale-auth-key)
      if [ "$#" -lt 2 ]; then
        echo "ERROR: --tailscale-auth-key requires a value" >&2
        exit 1
      fi
      TAILSCALE_AUTHKEY="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: missing required command: $1" >&2
    exit 1
  }
}

copy_required_file() {
  local src_rel="$1"
  local dst="$2"
  local mode="${3:-755}"
  local src="$SCRIPT_DIR/$src_rel"

  if [ ! -f "$src" ]; then
    echo "ERROR: missing required installer file: $src_rel" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$dst")"
  cp "$src" "$dst"
  chmod "$mode" "$dst"
}

required_files=(
  "api/commaview-api.py"
  "runtime/upgrade.sh"
  "runtime/commaview-supervisor.sh"
  "runtime/start.sh"
  "runtime/stop.sh"
  "runtime/uninstall.sh"
  "tailscale/install_tailscale_runtime.sh"
  "tailscale/tailscalectl.sh"
)

validate_required_files() {
  local missing=()
  local rel
  for rel in "${required_files[@]}"; do
    if [ ! -f "$SCRIPT_DIR/$rel" ]; then
      missing+=("$SCRIPT_DIR/$rel")
    fi
  done

  if [ "${#missing[@]}" -gt 0 ]; then
    echo "ERROR: missing required installer files:" >&2
    for path in "${missing[@]}"; do
      echo "  - $path" >&2
    done
    exit 1
  fi
}

deploy_required_scripts() {
  copy_required_file "api/commaview-api.py" "$INSTALL_DIR/api/commaview-api.py"
  copy_required_file "runtime/upgrade.sh" "$INSTALL_DIR/upgrade.sh"

  copy_required_file "runtime/commaview-supervisor.sh" "$INSTALL_DIR/commaview-supervisor.sh"
  copy_required_file "runtime/start.sh" "$INSTALL_DIR/start.sh"
  copy_required_file "runtime/stop.sh" "$INSTALL_DIR/stop.sh"
  copy_required_file "runtime/uninstall.sh" "$INSTALL_DIR/uninstall.sh"

  copy_required_file "tailscale/install_tailscale_runtime.sh" "$INSTALL_DIR/tailscale/install_tailscale_runtime.sh"
  copy_required_file "tailscale/tailscalectl.sh" "$INSTALL_DIR/tailscale/tailscalectl.sh"
}

need_cmd curl
need_cmd tar
need_cmd sha256sum

echo "=== CommaView ${VERSION} Installer ==="
echo "Release: ${RELEASE_TAG}"
echo "Repo:    ${GITHUB_REPO}"
if [ "$ENABLE_TAILSCALE" = "1" ]; then
  echo "Tailscale opt-in: enabled"
fi

tmpdir="$(mktemp -d /tmp/commaview-install.XXXXXX)"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

mkdir -p "$INSTALL_DIR/logs" "$INSTALL_DIR/run" "$INSTALL_DIR/lib" "$INSTALL_DIR/tailscale" "$INSTALL_DIR/api"

prompt_retry_or_continue() {
  if [ ! -t 0 ]; then
    echo "Non-interactive install; continuing without Tailscale." >&2
    return 1
  fi

  while true; do
    echo "Tailscale setup failed. Choose: [r]etry or [c]ontinue without Tailscale"
    read -r choice
    case "${choice:-}" in
      r|R|retry|Retry) return 0 ;;
      c|C|continue|Continue) return 1 ;;
      *) echo "Please enter r or c." ;;
    esac
  done
}

maybe_configure_tailscale_opt_in() {
  [ "$ENABLE_TAILSCALE" = "1" ] || return 0

  local runtime="$INSTALL_DIR/tailscale/install_tailscale_runtime.sh"
  local tailscale_bin="$INSTALL_DIR/tailscale/bin/tailscale"
  local tailscaled_bin="$INSTALL_DIR/tailscale/bin/tailscaled"
  local socket="$INSTALL_DIR/tailscale/state/tailscaled.sock"
  local statefile="$INSTALL_DIR/tailscale/state/tailscaled.state"

  mkdir -p "$INSTALL_DIR/tailscale/state" /data/params/d

  while true; do
    if ! "$runtime"; then
      echo "WARN: failed to install/check tailscale runtime" >&2
      if prompt_retry_or_continue; then
        continue
      fi
      printf "0" > /data/params/d/CommaViewTailscaleEnabled
      return 0
    fi

    if [ -z "${TAILSCALE_AUTHKEY:-}" ]; then
      if [ -t 0 ]; then
        echo "Enter one-time Tailscale auth key (or leave blank to continue without Tailscale):"
        read -r TAILSCALE_AUTHKEY
      fi
    fi

    if [ -z "${TAILSCALE_AUTHKEY:-}" ]; then
      echo "No auth key provided. Continuing without Tailscale setup."
      printf "0" > /data/params/d/CommaViewTailscaleEnabled
      return 0
    fi

    nohup nice -n 19 "$tailscaled_bin" --state="$statefile" --socket="$socket" >> "$INSTALL_DIR/logs/tailscale-install.log" 2>&1 &
    local tsd_pid=$!
    sleep 1

    if "$tailscale_bin" --socket "$socket" up --authkey "$TAILSCALE_AUTHKEY" --accept-routes --reset >> "$INSTALL_DIR/logs/tailscale-install.log" 2>&1; then
      "$tailscale_bin" --socket "$socket" down >> "$INSTALL_DIR/logs/tailscale-install.log" 2>&1 || true
      kill "$tsd_pid" 2>/dev/null || true
      wait "$tsd_pid" 2>/dev/null || true
      printf "1" > /data/params/d/CommaViewTailscaleEnabled
      unset TAILSCALE_AUTHKEY
      echo "Tailscale onboarding complete (auth key consumed once)."
      return 0
    fi

    kill "$tsd_pid" 2>/dev/null || true
    wait "$tsd_pid" 2>/dev/null || true

    if prompt_retry_or_continue; then
      continue
    fi

    printf "0" > /data/params/d/CommaViewTailscaleEnabled
    unset TAILSCALE_AUTHKEY
    return 0
  done
}

validate_required_files

echo "Stopping existing CommaView processes..."
pkill -f "commaview-supervisor.sh" 2>/dev/null || true
pkill -f "/data/commaview/commaview-bridge" 2>/dev/null || true
sleep 1

echo "Downloading release assets..."
curl -fL --retry 3 --retry-delay 1 -o "$tmpdir/$ASSET_NAME" "$BASE_URL/$ASSET_NAME"
curl -fL --retry 3 --retry-delay 1 -o "$tmpdir/$ASSET_SHA_NAME" "$BASE_URL/$ASSET_SHA_NAME"

expected_sha="$(awk 'NF{print $1; exit}' "$tmpdir/$ASSET_SHA_NAME" | tr -d '\r\n')"
if ! echo "$expected_sha" | grep -Eq '^[0-9a-fA-F]{64}$'; then
  echo "ERROR: invalid sha256 file format: $ASSET_SHA_NAME" >&2
  exit 1
fi
actual_sha="$(sha256sum "$tmpdir/$ASSET_NAME" | awk '{print $1}')"
if [ "$expected_sha" != "$actual_sha" ]; then
  echo "ERROR: checksum mismatch" >&2
  echo "  expected: $expected_sha" >&2
  echo "  actual:   $actual_sha" >&2
  exit 1
fi

echo "Extracting bundle..."
tar -xzf "$tmpdir/$ASSET_NAME" -C "$INSTALL_DIR" --strip-components=1

deploy_required_scripts

if [ ! -f "$INSTALL_DIR/commaview-bridge" ]; then
  echo "ERROR: bundle missing $INSTALL_DIR/commaview-bridge" >&2
  exit 1
fi
if [ ! -f "$INSTALL_DIR/lib/libcapnp-0.8.0.so" ] || [ ! -f "$INSTALL_DIR/lib/libkj-0.8.0.so" ]; then
  echo "ERROR: bundle missing required runtime libs in $INSTALL_DIR/lib" >&2
  exit 1
fi

chmod +x "$INSTALL_DIR/commaview-bridge"
BINARY_SIZE=$(ls -lh "$INSTALL_DIR/commaview-bridge" | awk '{print $5}')

# Hook into continue.sh
if [ -f "$CONTINUE_SH" ] && ! grep -q "$MARKER" "$CONTINUE_SH"; then
  sed -i "/^exec .*launch_openpilot/i\\
$MARKER\\
/data/commaview/start.sh &" "$CONTINUE_SH"
  echo "Boot hook installed"
elif grep -q "$MARKER" "$CONTINUE_SH" 2>/dev/null; then
  echo "Boot hook already present"
fi

maybe_configure_tailscale_opt_in

echo ""
echo "=== CommaView ${VERSION} installed ==="
echo "  Source:      ${BASE_URL}/${ASSET_NAME}"
echo "  Binary:      $INSTALL_DIR/commaview-bridge ($BINARY_SIZE)"
echo "  Supervisor:  single-process bridge watchdog + tailscale policy"
echo "  Bridge:      prod-only"
echo "  Runtime:     openpilot manager owns camerad/encoderd"
if [ "$ENABLE_TAILSCALE" = "1" ]; then
  echo "  Tailscale:   opt-in flow complete (see /data/params/d/CommaViewTailscaleEnabled)"
fi
echo "  Upgrade:     bash $INSTALL_DIR/upgrade.sh"
echo "  Uninstall:   bash $INSTALL_DIR/uninstall.sh"
