#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
PATCH_ROOT="$INSTALL_DIR/patches"
STATE_JSON="$INSTALL_DIR/run/onroad-ui-export-status.json"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"
JSON_ONLY=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --json) JSON_ONLY=1; shift ;;
    -h|--help) echo "Usage: verify_onroad_ui_export_patch.sh [--json]"; exit 0 ;;
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
    if [ "${ONROAD_UI_EXPORT_OP_ROOT:-}" = "$OP_ROOT" ] && [ -n "${ONROAD_UI_EXPORT_FLAVOR:-}" ] && [ -f "$PATCH_ROOT/$ONROAD_UI_EXPORT_FLAVOR/0001-commaview-ui-export-v2.patch" ]; then
      printf '%s\n' "$ONROAD_UI_EXPORT_FLAVOR"
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
    patch="$PATCH_ROOT/$flavor/0001-commaview-ui-export-v2.patch"
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
  return 1
}

if flavor="$(detect_flavor)"; then
  flavor_detected=1
else
  flavor=""
  flavor_detected=0
fi
patch="$PATCH_ROOT/$flavor/0001-commaview-ui-export-v2.patch"
state="stale"
status_scope="patch-installation"
reason="onroad UI export status unavailable"
healthy=false
patch_verified=false
repair_needed=true
control_service_present=false
scene_service_present=false
status_service_present=false
schema_present=false
runtime_flavor_field_present=false
status_mode_enum_present=false
status_mode_field_present=false
lat_active_field_present=false
long_active_field_present=false
control_publisher_present=false
scene_publisher_present=false
status_publisher_present=false
runtime_flavor_constant_present=false
runtime_flavor_publisher_present=false
status_mode_helper_present=false
status_mode_publisher_present=false
lat_long_publisher_present=false
control_event_present=false
scene_event_present=false
status_event_present=false
fingerprint=""

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  state="missing-repo"; reason="upstream repo not found at $OP_ROOT"
elif [ "$flavor_detected" -ne 1 ]; then
  state="unknown-flavor"; reason="unable to determine supported direct v2 patch flavor for $OP_ROOT"
elif [ ! -f "$patch" ]; then
  state="missing-patch"; reason="missing direct v2 patch asset for $flavor"
else
  fingerprint="$(sha256sum "$patch" | awk '{print $1}')"
  expected_runtime_flavor="UNKNOWN"
  case "$flavor" in
    openpilot) expected_runtime_flavor="OPENPILOT" ;;
    sunnypilot) expected_runtime_flavor="SUNNYPILOT" ;;
  esac
  grep -Fq '"commaViewControl"' "$OP_ROOT/cereal/services.py" && control_service_present=true || true
  grep -Fq '"commaViewScene"' "$OP_ROOT/cereal/services.py" && scene_service_present=true || true
  grep -Fq '"commaViewStatus"' "$OP_ROOT/cereal/services.py" && status_service_present=true || true
  grep -Fq 'struct CommaViewControl' "$OP_ROOT/cereal/commaview.capnp" && schema_present=true || true
  grep -Fq 'runtimeFlavor @18 :Text;' "$OP_ROOT/cereal/commaview.capnp" && runtime_flavor_field_present=true || true
  grep -Fq 'enum CommaViewStatusMode {' "$OP_ROOT/cereal/commaview.capnp" && status_mode_enum_present=true || true
  grep -Fq 'statusMode @19 :CommaViewStatusMode;' "$OP_ROOT/cereal/commaview.capnp" && status_mode_field_present=true || true
  grep -Fq 'latActive @14 :Bool;' "$OP_ROOT/cereal/commaview.capnp" && lat_active_field_present=true || true
  grep -Fq 'longActive @15 :Bool;' "$OP_ROOT/cereal/commaview.capnp" && long_active_field_present=true || true
  grep -Fq '_publish_commaview_control' "$OP_ROOT/selfdrive/ui/ui_state.py" && control_publisher_present=true || true
  grep -Fq '_publish_commaview_scene' "$OP_ROOT/selfdrive/ui/ui_state.py" && scene_publisher_present=true || true
  grep -Fq '_publish_commaview_status' "$OP_ROOT/selfdrive/ui/ui_state.py" && status_publisher_present=true || true
  grep -Fq "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$OP_ROOT/selfdrive/ui/ui_state.py" && runtime_flavor_constant_present=true || true
  grep -Fq 'status.runtimeFlavor = COMMAVIEW_RUNTIME_FLAVOR if COMMAVIEW_RUNTIME_FLAVOR in ("OPENPILOT", "SUNNYPILOT") else COMMAVIEW_RUNTIME_FLAVOR_UNKNOWN' "$OP_ROOT/selfdrive/ui/ui_state.py" && runtime_flavor_publisher_present=true || true
  grep -Fq 'def _commaview_status_mode_name(status) -> str:' "$OP_ROOT/selfdrive/ui/ui_state.py" && status_mode_helper_present=true || true
  grep -Fq 'status.statusMode = self._commaview_status_mode_name(self.status)' "$OP_ROOT/selfdrive/ui/ui_state.py" && status_mode_publisher_present=true || true
  if grep -Fq 'control.latActive = bool(car_control.latActive)' "$OP_ROOT/selfdrive/ui/ui_state.py" && grep -Fq 'control.longActive = bool(car_control.longActive)' "$OP_ROOT/selfdrive/ui/ui_state.py"; then
    lat_long_publisher_present=true
  fi
  grep -Fq 'commaViewControl @150' "$OP_ROOT/cereal/log.capnp" && control_event_present=true || true
  grep -Fq 'commaViewScene @151' "$OP_ROOT/cereal/log.capnp" && scene_event_present=true || true
  grep -Fq 'commaViewStatus @152' "$OP_ROOT/cereal/log.capnp" && status_event_present=true || true
  if $control_service_present && $scene_service_present && $status_service_present && $schema_present && \
     $runtime_flavor_field_present && $status_mode_enum_present && $status_mode_field_present && $lat_active_field_present && $long_active_field_present && \
     $control_publisher_present && $scene_publisher_present && $status_publisher_present && \
     $runtime_flavor_constant_present && $runtime_flavor_publisher_present && $status_mode_helper_present && $status_mode_publisher_present && $lat_long_publisher_present && \
     $control_event_present && $scene_event_present && $status_event_present; then
    state="patch-verified"
    reason="static direct v2 patch markers verified; runtime telemetry not proven"
    patch_verified=true
    repair_needed=false
  else
    state="repair-needed"
    reason="direct v2 export markers missing from upstream UI tree"
  fi
fi

json=$(printf '{"healthy":%s,"patchVerified":%s,"statusScope":"%s","repairNeeded":%s,"state":"%s","reason":"%s","flavor":"%s","opRoot":"%s","patch":"%s","patchFingerprint":"%s","controlServicePresent":%s,"sceneServicePresent":%s,"statusServicePresent":%s,"schemaPresent":%s,"runtimeFlavorFieldPresent":%s,"statusModeEnumPresent":%s,"statusModeFieldPresent":%s,"latActiveFieldPresent":%s,"longActiveFieldPresent":%s,"controlPublisherPresent":%s,"scenePublisherPresent":%s,"statusPublisherPresent":%s,"runtimeFlavorConstantPresent":%s,"runtimeFlavorPublisherPresent":%s,"statusModeHelperPresent":%s,"statusModePublisherPresent":%s,"latLongPublisherPresent":%s,"controlEventPresent":%s,"sceneEventPresent":%s,"statusEventPresent":%s}' "$healthy" "$patch_verified" "$status_scope" "$repair_needed" "$state" "$reason" "$flavor" "$OP_ROOT" "$patch" "$fingerprint" "$control_service_present" "$scene_service_present" "$status_service_present" "$schema_present" "$runtime_flavor_field_present" "$status_mode_enum_present" "$status_mode_field_present" "$lat_active_field_present" "$long_active_field_present" "$control_publisher_present" "$scene_publisher_present" "$status_publisher_present" "$runtime_flavor_constant_present" "$runtime_flavor_publisher_present" "$status_mode_helper_present" "$status_mode_publisher_present" "$lat_long_publisher_present" "$control_event_present" "$scene_event_present" "$status_event_present")
printf '%s\n' "$json" > "$STATE_JSON"
if [ -n "$fingerprint" ]; then
  printf 'ONROAD_UI_EXPORT_FLAVOR=%s\nONROAD_UI_EXPORT_PATCH_SHA=%s\nONROAD_UI_EXPORT_OP_ROOT=%s\n' "$flavor" "$fingerprint" "$OP_ROOT" > "$STATE_ENV"
fi
printf '%s\n' "$json"
if $patch_verified; then exit 0; fi
exit 1
