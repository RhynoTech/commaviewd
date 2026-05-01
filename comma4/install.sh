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

INSTALLED_VERSION=""
INSTALLED_RELEASE_TAG=""
if [ -f "$VERSION_ENV" ]; then
  # shellcheck disable=SC1090
  . "$VERSION_ENV"
  INSTALLED_VERSION="${VERSION:-}"
  INSTALLED_RELEASE_TAG="${RELEASE_TAG:-}"
fi

USE_CURRENT_RELEASE=0
RELEASE_TAG="${COMMAVIEWD_RELEASE_TAG:-}"
VERSION="${COMMAVIEWD_VERSION:-}"
ASSET_NAME=""
ASSET_SHA_NAME=""
BASE_URL=""
INSTALLER_REF=""
INSTALLER_RAW_BASE=""

resolve_release_inputs() {
  if [ -z "$RELEASE_TAG" ]; then
    if [ "$USE_CURRENT_RELEASE" = "1" ] && [ -n "$INSTALLED_RELEASE_TAG" ]; then
      RELEASE_TAG="$INSTALLED_RELEASE_TAG"
    elif [ -n "${COMMAVIEWD_DEFAULT_TAG:-}" ]; then
      RELEASE_TAG="$COMMAVIEWD_DEFAULT_TAG"
    elif [ -n "${COMMAVIEWD_INSTALLER_REF:-}" ] && [[ "${COMMAVIEWD_INSTALLER_REF}" == v* ]]; then
      RELEASE_TAG="$COMMAVIEWD_INSTALLER_REF"
    else
      RELEASE_TAG="$(resolve_latest_release_tag)"
    fi
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
}

resolve_release_inputs

INSTALL_DIR="/data/commaview"
CONTINUE_SH="/data/continue.sh"
MARKER="# commaview-hook"
PARAMS_DIR="/data/params/d"
FORCE_OFFROAD=0
FORCE_OFFROAD_OWNED=0
FORCE_OFFROAD_PREV=""
tmpdir=""
COMPANION_DIR="$SCRIPT_DIR"
INSTALL_MUTATED=0
INSTALL_SUCCESS=0

usage() {
  cat <<USAGE
CommaView installer ${VERSION}

Usage:
  install.sh [--tag <release-tag>] [--current] [--force-offroad] [--help]

Options:
  --tag <release-tag>            Install or update to a specific release tag.
  --current                      Reinstall the currently installed release instead of resolving latest.
  --force-offroad                Set OffroadMode and wait for an actual offroad transition before changing files.
  -h, --help                     Show this help and exit.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --tag)
      [ "$#" -ge 2 ] || { echo "ERROR: --tag requires a value" >&2; exit 1; }
      RELEASE_TAG="$2"
      VERSION=""
      resolve_release_inputs
      shift 2
      ;;
    --current)
      USE_CURRENT_RELEASE=1
      RELEASE_TAG=""
      VERSION=""
      resolve_release_inputs
      shift
      ;;
    --force-offroad)
      FORCE_OFFROAD=1
      shift
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

read_param() {
  local path="$PARAMS_DIR/$1"
  [ -f "$path" ] || return 0
  tr -d '\000\r\n' < "$path" 2>/dev/null || true
}

write_param() {
  mkdir -p "$PARAMS_DIR"
  printf '%s' "$2" > "$PARAMS_DIR/$1"
}

restore_force_offroad_mode() {
  if [ "$FORCE_OFFROAD_OWNED" = "1" ]; then
    write_param "OffroadMode" "${FORCE_OFFROAD_PREV:-0}"
  fi
}

restore_previous_install_tree() {
  local backup_dir="$tmpdir/previous-install"
  [ "$INSTALL_MUTATED" = "1" ] || return 0
  [ "$INSTALL_SUCCESS" = "0" ] || return 0
  [ -d "$backup_dir" ] || return 0

  echo "WARN: install failed after modifying live tree; restoring previous CommaView files" >&2
  rm -f \
    "$INSTALL_DIR/commaviewd" \
    "$INSTALL_DIR/VERSION" \
    "$INSTALL_DIR/start.sh" \
    "$INSTALL_DIR/stop.sh" \
    "$INSTALL_DIR/uninstall.sh" \
    "$INSTALL_DIR/runtime-debug.defaults.json" \
    "$INSTALL_DIR/version.env"
  rm -rf \
    "$INSTALL_DIR/lib" \
    "$INSTALL_DIR/scripts" \
    "$INSTALL_DIR/patches"
  cp -a "$backup_dir"/. "$INSTALL_DIR"/ 2>/dev/null || true
  if [ -x "$INSTALL_DIR/start.sh" ]; then
    echo "WARN: restarting restored CommaView runtime after failed install" >&2
    COMMAVIEWD_RESTART_REASON=install-rollback bash "$INSTALL_DIR/start.sh" >/dev/null 2>&1 || \
      echo "WARN: failed to restart restored CommaView runtime" >&2
  fi
}

cleanup() {
  restore_previous_install_tree
  restore_force_offroad_mode
  rm -rf "${tmpdir:-}"
}

wait_until_offroad() {
  local timeout_sec="${1:-45}"
  local elapsed=0
  local is_onroad=""
  while [ "$elapsed" -lt "$timeout_sec" ]; do
    is_onroad="$(read_param IsOnroad)"
    if [ "$is_onroad" != "1" ]; then
      return 0
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
  return 1
}

ensure_offroad_ready() {
  local is_onroad
  is_onroad="$(read_param IsOnroad)"
  if [ "$is_onroad" != "1" ]; then
    return 0
  fi

  if [ "$FORCE_OFFROAD" != "1" ]; then
    echo "ERROR: install blocked while onroad. Park the vehicle or rerun with --force-offroad." >&2
    exit 42
  fi

  FORCE_OFFROAD_PREV="$(read_param OffroadMode)"
  if [ "$FORCE_OFFROAD_PREV" != "1" ]; then
    echo "Requesting OffroadMode for maintenance..."
    write_param "OffroadMode" "1"
    FORCE_OFFROAD_OWNED=1
  fi

  echo "Waiting for actual offroad transition..."
  if ! wait_until_offroad 45; then
    echo "ERROR: device did not transition offroad in time" >&2
    exit 42
  fi
}

copy_required_file() {
  local src_rel="$1"
  local dst="$2"
  local mode="${3:-755}"
  local src="$COMPANION_DIR/$src_rel"

  if [ ! -f "$src" ]; then
    echo "ERROR: missing required installer file: $src_rel" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$dst")"
  cp "$src" "$dst"
  chmod "$mode" "$dst"
}

required_files=(
  "install.sh"
  "start.sh"
  "stop.sh"
  "uninstall.sh"
  "runtime-debug.defaults.json"
  "scripts/verify_onroad_ui_export_patch.sh"
  "scripts/apply_onroad_ui_export_patch.sh"
  "patches/openpilot/0001-commaview-ui-export-v2.patch"
  "patches/sunnypilot/0001-commaview-ui-export-v2.patch"
)

refresh_required_files() {
  local rel dst url dir
  local failed=()

  COMPANION_DIR="$tmpdir/companions"
  mkdir -p "$COMPANION_DIR"

  for rel in "${required_files[@]}"; do
    dst="$COMPANION_DIR/$rel"
    url="${INSTALLER_RAW_BASE}/$rel"
    dir="$(dirname "$dst")"

    if ! mkdir -p "$dir"; then
      echo "ERROR: unable to create directory for installer companion files: $dir" >&2
      failed+=("$rel")
      continue
    fi

    echo "Fetching installer companion: $rel"
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
    if [ ! -f "$COMPANION_DIR/$rel" ]; then
      missing+=("$COMPANION_DIR/$rel")
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
  copy_required_file "install.sh" "$INSTALL_DIR/install.sh"
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

print_pairing_code() {
  local token_path="/data/commaview/api/auth.token"
  local token=""
  local response=""
  local pair_code=""

  if [ -r "$token_path" ]; then
    token="$(tr -d '\r\n' < "$token_path")"
  fi

  if [ -z "$token" ]; then
    echo "Pair code: unavailable (missing API token)"
    return 0
  fi

  response="$(curl -fsS --retry 5 --retry-delay 1 -X POST \
    -H "X-CommaView-Token: $token" \
    http://127.0.0.1:5002/pairing/create 2>/dev/null || true)"
  pair_code="$(PAIRING_RESPONSE="$response" python3 - <<'PYPAIR'
import json
import os
try:
    data = json.loads(os.environ.get("PAIRING_RESPONSE", ""))
    print(data.get("pairCode", ""))
except Exception:
    print("")
PYPAIR
)"

  if [ -n "$pair_code" ]; then
    echo ""
    echo "CommaView pair code: $pair_code"
    echo "Enter this one-time pair code in the CommaView app."
  else
    echo "Pair code: unavailable (open CommaView settings to generate one after install)"
  fi
}

backup_managed_install_tree() {
  local backup_dir="$tmpdir/previous-install"
  mkdir -p "$backup_dir"
  for rel in \
    commaviewd \
    VERSION \
    start.sh \
    stop.sh \
    uninstall.sh \
    runtime-debug.defaults.json \
    version.env \
    lib \
    scripts \
    patches; do
    [ -e "$INSTALL_DIR/$rel" ] || continue
    cp -a "$INSTALL_DIR/$rel" "$backup_dir/$rel"
  done
}

clean_managed_install_tree() {
  echo "Removing stale managed CommaView files..."
  INSTALL_MUTATED=1
  rm -f \
    "$INSTALL_DIR/commaviewd" \
    "$INSTALL_DIR/VERSION" \
    "$INSTALL_DIR/start.sh" \
    "$INSTALL_DIR/stop.sh" \
    "$INSTALL_DIR/uninstall.sh" \
    "$INSTALL_DIR/runtime-debug.defaults.json" \
    "$INSTALL_DIR/version.env"
  rm -rf \
    "$INSTALL_DIR/lib" \
    "$INSTALL_DIR/scripts" \
    "$INSTALL_DIR/patches" \
    "$INSTALL_DIR/run"
  mkdir -p \
    "$INSTALL_DIR/logs" \
    "$INSTALL_DIR/run" \
    "$INSTALL_DIR/lib" \
    "$INSTALL_DIR/api" \
    "$INSTALL_DIR/config"
  rm -f \
    "$INSTALL_DIR/config/hud-lite-patch.env"
}

need_cmd curl
need_cmd tar
need_cmd sha256sum
need_cmd cp

tmpdir="$(mktemp -d /tmp/commaview-install.XXXXXX)"
trap cleanup EXIT
refresh_required_files
validate_required_files

echo "=== CommaView ${VERSION} Installer ==="
echo "Release: ${RELEASE_TAG}"
echo "Repo:    ${GITHUB_REPO}"

ensure_offroad_ready

mkdir -p "$INSTALL_DIR/logs" "$INSTALL_DIR/run" "$INSTALL_DIR/lib" "$INSTALL_DIR/api"

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

STAGED_BUNDLE="$tmpdir/staged-bundle"
mkdir -p "$STAGED_BUNDLE"
echo "Staging and validating bundle..."
tar -xzf "$tmpdir/$ASSET_NAME" -C "$STAGED_BUNDLE" --strip-components=1
if [ ! -f "$STAGED_BUNDLE/commaviewd" ]; then
  echo "ERROR: bundle missing commaviewd" >&2
  exit 1
fi
staged_capnp_lib_count=$(find "$STAGED_BUNDLE/lib" -maxdepth 1 -type f -name 'libcapnp-*.so' 2>/dev/null | wc -l | tr -d ' ')
staged_kj_lib_count=$(find "$STAGED_BUNDLE/lib" -maxdepth 1 -type f -name 'libkj-*.so' 2>/dev/null | wc -l | tr -d ' ')
if [ "$staged_capnp_lib_count" -eq 0 ] || [ "$staged_kj_lib_count" -eq 0 ]; then
  echo "ERROR: bundle missing required runtime libs" >&2
  exit 1
fi

backup_managed_install_tree

echo "Stopping existing CommaView processes..."
pkill -f "/data/commaview/commaviewd" 2>/dev/null || true
sleep 1

clean_managed_install_tree

echo "Installing staged bundle..."
cp -a "$STAGED_BUNDLE"/. "$INSTALL_DIR"/

deploy_required_scripts
ensure_api_auth_token
echo "Applying direct v2 onroad UI export patch lifecycle..."
if [ -x "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh" ]; then
  COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" bash "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh" --force-repair
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
print_pairing_code
INSTALL_SUCCESS=1

echo ""
echo "=== CommaView ${VERSION} installed ==="
echo "  Source:      ${BASE_URL}/${ASSET_NAME}"
echo "  Binary:      $INSTALL_DIR/commaviewd ($BINARY_SIZE)"
echo "  Runtime:     commaviewd dual-mode (bridge + control)"
echo "  Direct v2 onroad UI export: install-time patch lifecycle enforced"
echo "  Install/update: bash $INSTALL_DIR/install.sh [--tag <release-tag>] [--current] [--force-offroad]"
echo "  Uninstall:   bash $INSTALL_DIR/uninstall.sh"
