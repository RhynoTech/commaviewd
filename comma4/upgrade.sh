#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"
VERSION_ENV="${SCRIPT_DIR}/version.env"

DEFAULT_TAG="${COMMAVIEW_DEFAULT_TAG:-v0.0.1-alpha}"
if [ -f "$VERSION_ENV" ]; then
  # shellcheck disable=SC1090
  . "$VERSION_ENV"
  if [ -n "${RELEASE_TAG:-}" ]; then
    DEFAULT_TAG="$RELEASE_TAG"
  fi
fi

TAG="$DEFAULT_TAG"
GITHUB_REPO="${COMMAVIEW_RELEASE_REPO:-RhynoTech/commaviewd}"

usage() {
  cat <<USAGE
CommaView upgrade script

Usage:
  upgrade.sh [--tag <release-tag>] [--help]

Options:
  --tag <release-tag>  Override release tag (default: ${DEFAULT_TAG})
  -h, --help           Show help
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --tag)
      [ "$#" -ge 2 ] || { echo "ERROR: --tag requires a value" >&2; exit 1; }
      TAG="$2"
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

INSTALL_SCRIPT_URL_DEFAULT="https://raw.githubusercontent.com/${GITHUB_REPO}/${TAG}/comma4/install.sh"
INSTALL_SCRIPT_URL="${COMMAVIEW_INSTALL_SCRIPT_URL:-$INSTALL_SCRIPT_URL_DEFAULT}"

is_onroad="$(tr -d '\000\r\n' < /data/params/d/IsOnroad 2>/dev/null || echo 0)"
if [ "$is_onroad" = "1" ]; then
  echo "ERROR: Upgrade blocked while onroad (IsOnroad=1). Park the vehicle and retry." >&2
  exit 42
fi

ASSET_NAME="commaview-comma4-${TAG}.tar.gz"
ASSET_SHA_NAME="${ASSET_NAME}.sha256"
BASE_URL="https://github.com/${GITHUB_REPO}/releases/download/${TAG}"

tmpdir="$(mktemp -d /tmp/commaview-upgrade.XXXXXX)"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

echo "Preflight: validating release assets for ${TAG}"
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

echo "Stopping existing CommaView services..."
if [ -x /data/commaview/stop.sh ]; then
  bash /data/commaview/stop.sh || true
fi

install_script="$tmpdir/install.sh"
if ! curl -fL --retry 3 --retry-delay 1 -o "$install_script" "$INSTALL_SCRIPT_URL"; then
  fallback_url="https://raw.githubusercontent.com/${GITHUB_REPO}/master/comma4/install.sh"
  echo "WARN: failed to fetch install script at ${INSTALL_SCRIPT_URL}; falling back to ${fallback_url}" >&2
  curl -fL --retry 3 --retry-delay 1 -o "$install_script" "$fallback_url"
fi
chmod +x "$install_script"

echo "Running installer for ${TAG}"
COMMAVIEW_RELEASE_REPO="$GITHUB_REPO" \
COMMAVIEW_ASSET_NAME="$ASSET_NAME" \
COMMAVIEW_BASE_URL="$BASE_URL" \
COMMAVIEW_INSTALLER_REF="$TAG" \
bash "$install_script"

if [ -x /data/commaview/start.sh ]; then
  bash /data/commaview/start.sh || true
fi

echo "Upgrade complete: ${TAG}"
if [ -f /data/commaview/VERSION ]; then
  echo "Installed version: $(tr -d '\r\n' < /data/commaview/VERSION)"
fi

echo "If needed, rollback by re-running with an older tag:"
echo "  bash /data/commaview/upgrade.sh --tag <older-tag>"
