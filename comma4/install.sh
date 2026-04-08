#!/usr/bin/env bash
# CommaView installer for comma 4 (AGNOS/sunnypilot)
# Installs prebuilt C++ commaviewd bundle from pinned GitHub release.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"
VERSION_ENV="${SCRIPT_DIR}/version.env"
GITHUB_REPO="${COMMAVIEWD_RELEASE_REPO:-RhynoTech/commaviewd}"

resolve_latest_release_tag() {
  local api_url="https://api.github.com/repos/${GITHUB_REPO}/releases?per_page=20"
  curl -fsSL --retry 3 --retry-delay 1 "$api_url"     | tr -d '\r'     | grep -m1 '"tag_name":'     | sed -E 's/.*"tag_name":[[:space:]]*"([^"]+)".*/\1/' || true
}

if [ -f "$VERSION_ENV" ]; then
  # shellcheck disable=SC1090
  . "$VERSION_ENV"
fi

RELEASE_TAG="${COMMAVIEWD_RELEASE_TAG:-${RELEASE_TAG:-}}"
if [ -z "$RELEASE_TAG" ] && [ -n "${COMMAVIEWD_DEFAULT_TAG:-}" ]; then
  RELEASE_TAG="$COMMAVIEWD_DEFAULT_TAG"
fi
if [ -z "$RELEASE_TAG" ] && [ -n "${COMMAVIEWD_INSTALLER_REF:-}" ] && [[ "${COMMAVIEWD_INSTALLER_REF}" == v* ]]; then
  RELEASE_TAG="$COMMAVIEWD_INSTALLER_REF"
fi
if [ -z "$RELEASE_TAG" ]; then
  RELEASE_TAG="$(resolve_latest_release_tag)"
fi
if [ -z "$RELEASE_TAG" ]; then
  RELEASE_TAG="v0.0.1-alpha"
fi

VERSION="${COMMAVIEWD_VERSION:-${VERSION:-${RELEASE_TAG#v}}}"

ASSET_NAME="${COMMAVIEWD_ASSET_NAME:-commaview-comma4-${RELEASE_TAG}.tar.gz}"
ASSET_SHA_NAME="${ASSET_NAME}.sha256"
BASE_URL="${COMMAVIEWD_BASE_URL:-https://github.com/${GITHUB_REPO}/releases/download/${RELEASE_TAG}}"
# Keep installer companions pinned to the same resolved release by default.
# Falling back to master here mixes release assets with moving scripts/patches.
INSTALLER_REF="${COMMAVIEWD_INSTALLER_REF:-$RELEASE_TAG}"
INSTALLER_RAW_BASE="${COMMAVIEWD_INSTALLER_RAW_BASE:-https://raw.githubusercontent.com/${GITHUB_REPO}/${INSTALLER_REF}/comma4}"

INSTALL_DIR="/data/commaview"
CONTINUE_SH="/data/continue.sh"
MARKER="# commaview-hook"

usage() {
  cat <<USAGE
CommaView installer ${VERSION}

Usage:
  install.sh [--help]

Options:
  -h, --help                     Show this help and exit.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
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
  "upgrade.sh"
  "start.sh"
  "stop.sh"
  "uninstall.sh"
  "runtime-debug.defaults.json"
  "scripts/verify_onroad_ui_export_patch.sh"
  "scripts/apply_onroad_ui_export_patch.sh"
  "patches/openpilot/0001-commaview-ui-export-v2.patch"
  "patches/sunnypilot/0001-commaview-ui-export-v2.patch"
)

fetch_missing_required_files() {
  local rel dst url dir
  local failed=()

  for rel in "${required_files[@]}"; do
    dst="$SCRIPT_DIR/$rel"
    [ -f "$dst" ] && continue

    url="${INSTALLER_RAW_BASE}/$rel"
    dir="$(dirname "$dst")"

    if ! mkdir -p "$dir"; then
      echo "ERROR: unable to create directory for installer companion files: $dir" >&2
      failed+=("$rel")
      continue
    fi

    echo "Fetching missing installer companion: $rel"
    if ! curl -fsSL --retry 3 --retry-delay 1 -o "$dst" "$url"; then
      rm -f "$dst"
      failed+=("$rel")
    fi
  done

  if [ "${#failed[@]}" -gt 0 ]; then
    echo "ERROR: failed to fetch required installer files from ${INSTALLER_RAW_BASE}:" >&2
    for rel in "${failed[@]}"; do
      echo "  - $rel" >&2
    done
    exit 1
  fi
}

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
  copy_required_file "upgrade.sh" "$INSTALL_DIR/upgrade.sh"
  copy_required_file "start.sh" "$INSTALL_DIR/start.sh"
  copy_required_file "runtime-debug.defaults.json" "$INSTALL_DIR/runtime-debug.defaults.json" 644
  copy_required_file "scripts/verify_onroad_ui_export_patch.sh" "$INSTALL_DIR/scripts/verify_onroad_ui_export_patch.sh"
  copy_required_file "scripts/apply_onroad_ui_export_patch.sh" "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh"
  copy_required_file "patches/openpilot/0001-commaview-ui-export-v2.patch" "$INSTALL_DIR/patches/openpilot/0001-commaview-ui-export-v2.patch" 644
  copy_required_file "patches/sunnypilot/0001-commaview-ui-export-v2.patch" "$INSTALL_DIR/patches/sunnypilot/0001-commaview-ui-export-v2.patch" 644
  copy_required_file "stop.sh" "$INSTALL_DIR/stop.sh"
  copy_required_file "uninstall.sh" "$INSTALL_DIR/uninstall.sh"

  cat > "$INSTALL_DIR/version.env" <<EOF
VERSION="${VERSION}"
RELEASE_TAG="${RELEASE_TAG}"
EOF
  chmod 644 "$INSTALL_DIR/version.env"
}


ensure_api_auth_token() {
  local token_path="/data/commaview/api/auth.token"
  if [ -s "$token_path" ]; then
    chmod 600 "$token_path" 2>/dev/null || true
    return 0
  fi

  echo "Generating CommaView API auth token..."
  umask 077
  python3 - <<'PYTOKEN' > "$token_path"
import secrets
print(secrets.token_urlsafe(32))
PYTOKEN
  chmod 600 "$token_path" 2>/dev/null || true
}

need_cmd curl
need_cmd tar
need_cmd sha256sum

fetch_missing_required_files
validate_required_files

echo "=== CommaView ${VERSION} Installer ==="
echo "Release: ${RELEASE_TAG}"
echo "Repo:    ${GITHUB_REPO}"

tmpdir="$(mktemp -d /tmp/commaview-install.XXXXXX)"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

mkdir -p "$INSTALL_DIR/logs" "$INSTALL_DIR/run" "$INSTALL_DIR/lib" "$INSTALL_DIR/api"

echo "Stopping existing CommaView processes..."
pkill -f "/data/commaview/commaviewd" 2>/dev/null || true
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
ensure_api_auth_token
echo "Applying direct v2 onroad UI export patch lifecycle..."
if [ -x "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh" ]; then
  COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" bash "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh"
else
  echo "ERROR: missing onroad UI export patch apply helper" >&2
  exit 1
fi

if [ ! -f "$INSTALL_DIR/commaviewd" ]; then
  echo "ERROR: bundle missing $INSTALL_DIR/commaviewd" >&2
  exit 1
fi
capnp_lib_count=$(find "$INSTALL_DIR/lib" -maxdepth 1 -type f -name 'libcapnp-*.so' | wc -l | tr -d ' ')
kj_lib_count=$(find "$INSTALL_DIR/lib" -maxdepth 1 -type f -name 'libkj-*.so' | wc -l | tr -d ' ')
if [ "$capnp_lib_count" -eq 0 ] || [ "$kj_lib_count" -eq 0 ]; then
  echo "ERROR: bundle missing required runtime libs in $INSTALL_DIR/lib" >&2
  exit 1
fi

chmod +x "$INSTALL_DIR/commaviewd"
BINARY_SIZE=$(ls -lh "$INSTALL_DIR/commaviewd" | awk '{print $5}')

# Hook into continue.sh
if [ -f "$CONTINUE_SH" ] && ! grep -q "$MARKER" "$CONTINUE_SH"; then
  sed -i "/^exec .*launch_openpilot/i\\
$MARKER\\
/data/commaview/start.sh &" "$CONTINUE_SH"
  echo "Boot hook installed"
elif grep -q "$MARKER" "$CONTINUE_SH" 2>/dev/null; then
  echo "Boot hook already present"
fi

echo "Starting CommaView runtime..."
bash "$INSTALL_DIR/start.sh"
sleep 1

echo ""
echo "=== CommaView ${VERSION} installed ==="
echo "  Source:      ${BASE_URL}/${ASSET_NAME}"
echo "  Binary:      $INSTALL_DIR/commaviewd ($BINARY_SIZE)"
echo "  Runtime:     commaviewd dual-mode (bridge + control)"
echo "  Direct v2 onroad UI export: install-time patch lifecycle enforced"
echo "  Upgrade:     bash $INSTALL_DIR/upgrade.sh"
echo "  Uninstall:   bash $INSTALL_DIR/uninstall.sh"
