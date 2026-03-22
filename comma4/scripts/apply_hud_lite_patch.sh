#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
PATCH_ROOT="$INSTALL_DIR/patches"
VERIFY_SCRIPT="$INSTALL_DIR/scripts/verify_hud_lite_patch.sh"
STATE_ENV="$INSTALL_DIR/config/hud-lite-patch.env"

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help) echo "Usage: apply_hud_lite_patch.sh"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

is_onroad="$(tr -d "\000\r\n" < /data/params/d/IsOnroad 2>/dev/null || echo 0)"
if [ "$is_onroad" = "1" ]; then
  echo "ERROR: HUD-lite patch apply blocked while onroad" >&2
  exit 42
fi

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "ERROR: upstream repo not found at $OP_ROOT" >&2
  exit 1
fi

flavor="openpilot"
remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
if printf "%s" "$remote" | grep -qi "sunnypilot"; then flavor="sunnypilot"; fi
patch="$PATCH_ROOT/$flavor/0001-hud-lite-export.patch"
[ -f "$patch" ] || { echo "ERROR: missing HUD-lite patch asset: $patch" >&2; exit 1; }

if [ -x "$VERIFY_SCRIPT" ] && "$VERIFY_SCRIPT" --json >/dev/null 2>&1; then
  exit 0
fi

if ! git -C "$OP_ROOT" apply --reverse --check "$patch" >/dev/null 2>&1; then
  git -C "$OP_ROOT" apply --check "$patch"
  git -C "$OP_ROOT" apply "$patch"
fi

fingerprint="$(sha256sum "$patch" | awk "{print \$1}")"
mkdir -p "$(dirname "$STATE_ENV")"
printf "HUD_LITE_FLAVOR=%s\nHUD_LITE_PATCH_SHA=%s\nHUD_LITE_OP_ROOT=%s\n" "$flavor" "$fingerprint" "$OP_ROOT" > "$STATE_ENV"

if [ -x "$VERIFY_SCRIPT" ]; then
  exec "$VERIFY_SCRIPT" --json
fi
