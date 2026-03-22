#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
PATCH_ROOT="$INSTALL_DIR/patches"
STATE_JSON="$INSTALL_DIR/run/hud-lite-status.json"
STATE_ENV="$INSTALL_DIR/config/hud-lite-patch.env"
JSON_ONLY=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --json) JSON_ONLY=1; shift ;;
    -h|--help) echo "Usage: verify_hud_lite_patch.sh [--json]"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

mkdir -p "$(dirname "$STATE_JSON")" "$(dirname "$STATE_ENV")"

detect_flavor() {
  local preferred=""
  local remote=""
  local flavor=""
  local patch=""
  local matches=""

  if [ -f "$STATE_ENV" ]; then
    # shellcheck disable=SC1090
    . "$STATE_ENV" || true
    if [ "${HUD_LITE_OP_ROOT:-}" = "$OP_ROOT" ] && [ -n "${HUD_LITE_FLAVOR:-}" ] && [ -f "$PATCH_ROOT/$HUD_LITE_FLAVOR/0001-hud-lite-export.patch" ]; then
      printf '%s\n' "$HUD_LITE_FLAVOR"
      return 0
    fi
  fi

  if git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
    if printf '%s' "$remote" | grep -qi 'sunnypilot'; then
      preferred='sunnypilot'
    elif printf '%s' "$remote" | grep -qi 'openpilot'; then
      preferred='openpilot'
    fi
  fi

  for flavor in openpilot sunnypilot; do
    patch="$PATCH_ROOT/$flavor/0001-hud-lite-export.patch"
    [ -f "$patch" ] || continue
    if git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1 && \
       (git -C "$OP_ROOT" apply --reverse --check "$patch" >/dev/null 2>&1 || \
        git -C "$OP_ROOT" apply --check "$patch" >/dev/null 2>&1); then
      matches="$matches $flavor"
    fi
  done

  set -- $matches
  if [ "$#" -eq 1 ]; then
    printf '%s\n' "$1"
    return 0
  fi
  if [ "$#" -gt 1 ] && [ -n "$preferred" ]; then
    for flavor in "$@"; do
      if [ "$flavor" = "$preferred" ]; then
        printf '%s\n' "$preferred"
        return 0
      fi
    done
  fi
  if [ -n "$preferred" ] && [ -f "$PATCH_ROOT/$preferred/0001-hud-lite-export.patch" ]; then
    printf '%s\n' "$preferred"
    return 0
  fi
  return 1
}

if flavor="$(detect_flavor)"; then
  flavor_detected=1
else
  flavor=""
  flavor_detected=0
fi
patch="$PATCH_ROOT/$flavor/0001-hud-lite-export.patch"
state="stale"
status_scope="patch-installation"
reason="hud-lite status unavailable"
healthy=false
patch_verified=false
repair_needed=true
service_present=false
struct_present=false
publisher_present=false
log_event_present=false
fingerprint=""

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  state="missing-repo"; reason="upstream repo not found at $OP_ROOT"
elif [ "$flavor_detected" -ne 1 ]; then
  state="unknown-flavor"; reason="unable to determine supported HUD-lite patch flavor for $OP_ROOT"
elif [ ! -f "$patch" ]; then
  state="missing-patch"; reason="missing HUD-lite patch asset for $flavor"
else
  fingerprint="$(sha256sum "$patch" | awk '{print $1}')"
  grep -Fq "commaViewHudLite" "$OP_ROOT/cereal/services.py" && service_present=true || true
  grep -Fq "struct CommaViewHudLite" "$OP_ROOT/cereal/custom.capnp" && struct_present=true || true
  grep -Fq "COMMAVIEW HUD-LITE EXPORT" "$OP_ROOT/selfdrive/ui/ui_state.py" && publisher_present=true || true
  grep -Fq "commaViewHudLite" "$OP_ROOT/cereal/log.capnp" && log_event_present=true || true
  if $service_present && $struct_present && $publisher_present && $log_event_present; then
    state="patch-verified"
    reason="static patch markers verified; runtime telemetry not proven"
    patch_verified=true
    repair_needed=false
  else
    state="repair-needed"
    reason="HUD-lite export markers missing from upstream UI tree"
  fi
fi

json=$(printf '{"healthy":%s,"patchVerified":%s,"statusScope":"%s","repairNeeded":%s,"state":"%s","reason":"%s","flavor":"%s","opRoot":"%s","patch":"%s","patchFingerprint":"%s","servicePresent":%s,"structPresent":%s,"publisherPresent":%s,"logEventPresent":%s}' "$healthy" "$patch_verified" "$status_scope" "$repair_needed" "$state" "$reason" "$flavor" "$OP_ROOT" "$patch" "$fingerprint" "$service_present" "$struct_present" "$publisher_present" "$log_event_present")
printf '%s\n' "$json" > "$STATE_JSON"
if [ -n "$fingerprint" ]; then
  printf 'HUD_LITE_FLAVOR=%s\nHUD_LITE_PATCH_SHA=%s\nHUD_LITE_OP_ROOT=%s\n' "$flavor" "$fingerprint" "$OP_ROOT" > "$STATE_ENV"
fi
printf '%s\n' "$json"
if $patch_verified; then exit 0; fi
exit 1
