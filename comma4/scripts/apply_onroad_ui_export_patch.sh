#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
PATCH_ROOT="$INSTALL_DIR/patches"
VERIFY_SCRIPT="$INSTALL_DIR/scripts/verify_onroad_ui_export_patch.sh"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"

while [ "$#" -gt 0 ]; do
  case "$1" in
    -h|--help) echo "Usage: apply_onroad_ui_export_patch.sh"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

is_onroad="$(cat /data/params/d/IsOnroad 2>/dev/null | tr -d "\000\r\n" || echo 0)"
if [ "$is_onroad" = "1" ]; then
  echo "ERROR: socket UI export patch apply blocked while onroad" >&2
  exit 42
fi

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "ERROR: upstream repo not found at $OP_ROOT" >&2
  exit 1
fi

detect_flavor() {
  local preferred=""
  local remote=""
  local flavor=""
  local patch=""
  local matches=""

  if [ -f "$STATE_ENV" ]; then
    # shellcheck disable=SC1090
    . "$STATE_ENV" || true
    if [ "${ONROAD_UI_EXPORT_OP_ROOT:-}" = "$OP_ROOT" ] && [ -n "${ONROAD_UI_EXPORT_FLAVOR:-}" ] && [ -f "$PATCH_ROOT/$ONROAD_UI_EXPORT_FLAVOR/0001-commaview-ui-export-v2.patch" ]; then
      printf '%s
' "$ONROAD_UI_EXPORT_FLAVOR"
      return 0
    fi
  fi

  remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
  if printf '%s' "$remote" | grep -qi 'sunnypilot'; then
    preferred='sunnypilot'
  elif printf '%s' "$remote" | grep -qi 'openpilot'; then
    preferred='openpilot'
  fi

  for flavor in openpilot sunnypilot; do
    patch="$PATCH_ROOT/$flavor/0001-commaview-ui-export-v2.patch"
    [ -f "$patch" ] || continue
    if git -C "$OP_ROOT" apply --recount --reverse --check "$patch" >/dev/null 2>&1 ||        git -C "$OP_ROOT" apply --recount --check "$patch" >/dev/null 2>&1; then
      matches="$matches $flavor"
    fi
  done

  set -- $matches
  if [ "$#" -eq 1 ]; then
    printf '%s
' "$1"
    return 0
  fi
  if [ "$#" -gt 1 ] && [ -n "$preferred" ]; then
    for flavor in "$@"; do
      if [ "$flavor" = "$preferred" ]; then
        printf '%s
' "$preferred"
        return 0
      fi
    done
  fi
  return 1
}

flavor="$(detect_flavor)" || {
  echo "ERROR: unable to determine supported socket UI export patch flavor for $OP_ROOT" >&2
  exit 1
}
patch="$PATCH_ROOT/$flavor/0001-commaview-ui-export-v2.patch"
[ -f "$patch" ] || { echo "ERROR: missing socket UI export patch asset: $patch" >&2; exit 1; }

if [ -x "$VERIFY_SCRIPT" ] && "$VERIFY_SCRIPT" --json >/dev/null 2>&1; then
  exit 0
fi

if ! git -C "$OP_ROOT" apply --recount --reverse --check "$patch" >/dev/null 2>&1; then
  git -C "$OP_ROOT" apply --recount --check "$patch"
  git -C "$OP_ROOT" apply --recount "$patch"
fi

fingerprint="$(sha256sum "$patch" | awk '{print $1}')"
mkdir -p "$(dirname "$STATE_ENV")"
printf 'ONROAD_UI_EXPORT_FLAVOR=%s
ONROAD_UI_EXPORT_PATCH_SHA=%s
ONROAD_UI_EXPORT_OP_ROOT=%s
' "$flavor" "$fingerprint" "$OP_ROOT" > "$STATE_ENV"

if [ -x "$VERIFY_SCRIPT" ]; then
  exec "$VERIFY_SCRIPT" --json
fi
