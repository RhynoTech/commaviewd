#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
PATCH_ROOT="$INSTALL_DIR/patches"
STATE_JSON="$INSTALL_DIR/run/onroad-ui-export-status.json"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"
HELPER_PATH="$OP_ROOT/selfdrive/ui/commaview_export.py"
UI_STATE_PATH="$OP_ROOT/selfdrive/ui/ui_state.py"
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
       (git -C "$OP_ROOT" apply --recount --reverse --check "$patch" >/dev/null 2>&1 || \
        git -C "$OP_ROOT" apply --recount --check "$patch" >/dev/null 2>&1); then
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
helper_present=false
ui_state_hook_present=false
runtime_flavor_constant_present=false
socket_path_present=false
socket_env_present=false
frame_version_present=false
unix_socket_present=false
framing_present=false
control_payload_present=false
scene_payload_present=false
status_payload_present=false
control_publish_present=false
scene_publish_present=false
status_publish_present=false
cruise_set_speed_present=false
driver_monitoring_present=false
active_camera_present=false
status_mode_present=false
runtime_flavor_export_present=false
exporter_install_present=false
exporter_publish_present=false
speed_limit_helper_present=false
fingerprint=""

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  state="missing-repo"; reason="upstream repo not found at $OP_ROOT"
elif [ "$flavor_detected" -ne 1 ]; then
  state="unknown-flavor"; reason="unable to determine supported socket UI export patch flavor for $OP_ROOT"
elif [ ! -f "$patch" ]; then
  state="missing-patch"; reason="missing socket UI export patch asset for $flavor"
else
  fingerprint="$(sha256sum "$patch" | awk '{print $1}')"
  expected_runtime_flavor="OPENPILOT"
  if [ "$flavor" = "sunnypilot" ]; then
    expected_runtime_flavor="SUNNYPILOT"
  fi

  grep -Fq 'from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR' "$UI_STATE_PATH" && ui_state_hook_present=true || true
  [ -f "$HELPER_PATH" ] && helper_present=true || true
  grep -Fq "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$HELPER_PATH" && runtime_flavor_constant_present=true || true
  grep -Fq 'COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"' "$HELPER_PATH" && socket_path_present=true || true
  grep -Fq 'os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT' "$HELPER_PATH" && socket_env_present=true || true
  grep -Fq 'COMMAVIEW_FRAME_VERSION = 1' "$HELPER_PATH" && frame_version_present=true || true
  grep -Fq 'socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)' "$HELPER_PATH" && unix_socket_present=true || true
  grep -Fq 'struct.pack(">I", len(frame)) + frame' "$HELPER_PATH" && framing_present=true || true
  grep -Fq 'def _control_payload(self, ui_state) -> dict:' "$HELPER_PATH" && control_payload_present=true || true
  grep -Fq 'def _scene_payload(self, ui_state) -> dict:' "$HELPER_PATH" && scene_payload_present=true || true
  grep -Fq 'def _status_payload(self, ui_state) -> dict:' "$HELPER_PATH" && status_payload_present=true || true
  grep -Fq 'self._send_json(COMMAVIEW_CONTROL_SERVICE_INDEX, self._control_payload(ui_state))' "$HELPER_PATH" && control_publish_present=true || true
  grep -Fq 'self._send_json(COMMAVIEW_SCENE_SERVICE_INDEX, self._scene_payload(ui_state))' "$HELPER_PATH" && scene_publish_present=true || true
  grep -Fq 'self._send_json(COMMAVIEW_STATUS_SERVICE_INDEX, self._status_payload(ui_state))' "$HELPER_PATH" && status_publish_present=true || true
  grep -Fq '"cruiseSetSpeedMps":' "$HELPER_PATH" && cruise_set_speed_present=true || true
  grep -Fq '"driverMonitoring": {' "$HELPER_PATH" && driver_monitoring_present=true || true
  grep -Fq '"activeCamera": active_camera' "$HELPER_PATH" && active_camera_present=true || true
  grep -Fq '"statusMode": _status_mode_name(ui_state.status)' "$HELPER_PATH" && status_mode_present=true || true
  grep -Fq '"runtimeFlavor": self._flavor' "$HELPER_PATH" && runtime_flavor_export_present=true || true
  grep -Fq 'self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)' "$UI_STATE_PATH" && exporter_install_present=true || true
  grep -Fq 'self._commaview_exporter.publish(self)' "$UI_STATE_PATH" && exporter_publish_present=true || true
  if [ "$flavor" = "sunnypilot" ]; then
    if grep -Fq 'def _speed_limit_pre_active_icon(self, ui_state) -> str:' "$HELPER_PATH" && \
       grep -Fq 'custom.LongitudinalPlanSP.SpeedLimit.AssistState.preActive' "$HELPER_PATH"; then
      speed_limit_helper_present=true
    fi
  else
    speed_limit_helper_present=true
  fi

  if $helper_present && $ui_state_hook_present && $runtime_flavor_constant_present && \
     $socket_path_present && $socket_env_present && $frame_version_present && $unix_socket_present && $framing_present && \
     $control_payload_present && $scene_payload_present && $status_payload_present && \
     $control_publish_present && $scene_publish_present && $status_publish_present && \
     $cruise_set_speed_present && $driver_monitoring_present && $active_camera_present && \
     $status_mode_present && $runtime_flavor_export_present && $exporter_install_present && \
     $exporter_publish_present && $speed_limit_helper_present; then
    state="patch-verified"
    reason="socket UI export direct wiring markers verified; runtime telemetry not proven"
    patch_verified=true
    repair_needed=false
  else
    state="repair-needed"
    reason="socket UI export markers missing from upstream UI tree"
  fi
fi

json=$(printf '{"healthy":%s,"patchVerified":%s,"statusScope":"%s","repairNeeded":%s,"state":"%s","reason":"%s","flavor":"%s","opRoot":"%s","patch":"%s","patchFingerprint":"%s","helperPresent":%s,"uiStateHookPresent":%s,"runtimeFlavorConstantPresent":%s,"socketPathPresent":%s,"socketEnvPresent":%s,"frameVersionPresent":%s,"unixSocketPresent":%s,"framingPresent":%s,"controlPayloadPresent":%s,"scenePayloadPresent":%s,"statusPayloadPresent":%s,"controlPublishPresent":%s,"scenePublishPresent":%s,"statusPublishPresent":%s,"cruiseSetSpeedPresent":%s,"driverMonitoringPresent":%s,"activeCameraPresent":%s,"statusModePresent":%s,"runtimeFlavorExportPresent":%s,"exporterInstallPresent":%s,"exporterPublishPresent":%s,"speedLimitHelperPresent":%s}' "$healthy" "$patch_verified" "$status_scope" "$repair_needed" "$state" "$reason" "$flavor" "$OP_ROOT" "$patch" "$fingerprint" "$helper_present" "$ui_state_hook_present" "$runtime_flavor_constant_present" "$socket_path_present" "$socket_env_present" "$frame_version_present" "$unix_socket_present" "$framing_present" "$control_payload_present" "$scene_payload_present" "$status_payload_present" "$control_publish_present" "$scene_publish_present" "$status_publish_present" "$cruise_set_speed_present" "$driver_monitoring_present" "$active_camera_present" "$status_mode_present" "$runtime_flavor_export_present" "$exporter_install_present" "$exporter_publish_present" "$speed_limit_helper_present")
printf '%s\n' "$json" > "$STATE_JSON"
if [ -n "$fingerprint" ]; then
  printf 'ONROAD_UI_EXPORT_FLAVOR=%s\nONROAD_UI_EXPORT_PATCH_SHA=%s\nONROAD_UI_EXPORT_OP_ROOT=%s\n' "$flavor" "$fingerprint" "$OP_ROOT" > "$STATE_ENV"
fi
printf '%s\n' "$json"
if $patch_verified; then exit 0; fi
exit 1
