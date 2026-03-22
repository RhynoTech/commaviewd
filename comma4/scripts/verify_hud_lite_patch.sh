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

flavor="openpilot"
if git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
  if printf "%s" "$remote" | grep -qi "sunnypilot"; then flavor="sunnypilot"; fi
fi

patch="$PATCH_ROOT/$flavor/0001-hud-lite-export.patch"
state="healthy"
reason=""
healthy=true
repair_needed=false
service_present=false
struct_present=false
publisher_present=false
fingerprint=""

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  state="missing-repo"; reason="upstream repo not found at $OP_ROOT"; healthy=false; repair_needed=true
elif [ ! -f "$patch" ]; then
  state="missing-patch"; reason="missing HUD-lite patch asset for $flavor"; healthy=false; repair_needed=true
else
  fingerprint="$(sha256sum "$patch" | awk "{print \$1}")"
  grep -Fq "commaViewHudLite" "$OP_ROOT/cereal/services.py" && service_present=true || true
  grep -Fq "struct CommaViewHudLite" "$OP_ROOT/cereal/custom.capnp" && struct_present=true || true
  grep -Fq "COMMAVIEW HUD-LITE EXPORT" "$OP_ROOT/selfdrive/ui/ui_state.py" && publisher_present=true || true
  if ! $service_present || ! $struct_present || ! $publisher_present; then
    state="repair-needed"; reason="HUD-lite export markers missing from upstream UI tree"; healthy=false; repair_needed=true
  fi
fi

json=$(printf "{\"healthy\":%s,\"repairNeeded\":%s,\"state\":\"%s\",\"reason\":\"%s\",\"flavor\":\"%s\",\"opRoot\":\"%s\",\"patch\":\"%s\",\"patchFingerprint\":\"%s\",\"servicePresent\":%s,\"structPresent\":%s,\"publisherPresent\":%s}" "$healthy" "$repair_needed" "$state" "$reason" "$flavor" "$OP_ROOT" "$patch" "$fingerprint" "$service_present" "$struct_present" "$publisher_present")
printf "%s\n" "$json" > "$STATE_JSON"
if [ -n "$fingerprint" ]; then
  printf "HUD_LITE_FLAVOR=%s\nHUD_LITE_PATCH_SHA=%s\nHUD_LITE_OP_ROOT=%s\n" "$flavor" "$fingerprint" "$OP_ROOT" > "$STATE_ENV"
fi
printf "%s\n" "$json"
if $healthy; then exit 0; fi
exit 1
