#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
PATCH_ROOT="$INSTALL_DIR/patches"
STATE_JSON="$INSTALL_DIR/run/onroad-ui-export-status.json"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"
HELPER_PATH="$OP_ROOT/selfdrive/ui/commaview_export.py"
UI_STATE_PATH="$OP_ROOT/selfdrive/ui/ui_state.py"
AUGMENTED_ROAD_PATH="$OP_ROOT/selfdrive/ui/mici/onroad/augmented_road_view.py"
JSON_ONLY=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --json) JSON_ONLY=1; shift ;;
    -h|--help) echo "Usage: verify_onroad_ui_export_patch.sh [--json]"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

mkdir -p "$(dirname "$STATE_JSON")" "$(dirname "$STATE_ENV")"

check_fixed() {
  local needle="$1"
  local file="$2"
  grep -Fq -- "$needle" "$file"
}

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
compact_json_present=false
payload_helpers_present=false
publish_paths_present=false
risk_fields_present=false
legacy_bucket_markers_absent=false
exporter_install_present=false
exporter_publish_present=false
onroad_camera_relay_present=false
fingerprint=""
service_marker_count=0

service_consts=(
  COMMAVIEW_UI_STATE_ONROAD_SERVICE_INDEX
  COMMAVIEW_SELFDRIVE_STATE_SERVICE_INDEX
  COMMAVIEW_CAR_STATE_SERVICE_INDEX
  COMMAVIEW_CONTROLS_STATE_SERVICE_INDEX
  COMMAVIEW_ONROAD_EVENTS_SERVICE_INDEX
  COMMAVIEW_DRIVER_MONITORING_STATE_SERVICE_INDEX
  COMMAVIEW_DRIVER_STATE_V2_SERVICE_INDEX
  COMMAVIEW_MODEL_V2_SERVICE_INDEX
  COMMAVIEW_RADAR_STATE_SERVICE_INDEX
  COMMAVIEW_LIVE_CALIBRATION_SERVICE_INDEX
  COMMAVIEW_CAR_OUTPUT_SERVICE_INDEX
  COMMAVIEW_CAR_CONTROL_SERVICE_INDEX
  COMMAVIEW_LIVE_PARAMETERS_SERVICE_INDEX
  COMMAVIEW_LONGITUDINAL_PLAN_SERVICE_INDEX
  COMMAVIEW_CAR_PARAMS_SERVICE_INDEX
  COMMAVIEW_DEVICE_STATE_SERVICE_INDEX
  COMMAVIEW_ROAD_CAMERA_STATE_SERVICE_INDEX
  COMMAVIEW_PANDA_STATES_SUMMARY_SERVICE_INDEX
)

payload_markers=(
  'def _ui_state_onroad_payload(self, ui_state) -> dict:'
  'def _selfdrive_state_payload(self, ui_state) -> dict:'
  'def _car_state_payload(self, ui_state) -> dict:'
  'def _controls_state_payload(self, ui_state) -> dict:'
  'def _onroad_events_payload(self, ui_state) -> dict:'
  'def _driver_monitoring_state_payload(self, ui_state) -> dict:'
  'def _driver_state_v2_payload(self, ui_state) -> dict:'
  'def _model_v2_payload(self, ui_state) -> dict:'
  'def _radar_state_payload(self, ui_state) -> dict:'
  'def _live_calibration_payload(self, ui_state) -> dict:'
  'def _car_output_payload(self, ui_state) -> dict:'
  'def _car_control_payload(self, ui_state) -> dict:'
  'def _live_parameters_payload(self, ui_state) -> dict:'
  'def _longitudinal_plan_payload(self, ui_state) -> dict:'
  'def _car_params_payload(self, ui_state) -> dict:'
  'def _device_state_payload(self, ui_state) -> dict:'
  'def _road_camera_state_payload(self, ui_state) -> dict:'
  'def _panda_states_summary_payload(self, ui_state) -> dict:'
)

risk_markers=(
  '"alertHudVisual": 0'
  '"allowThrottle": False'
  '"openpilotLongitudinalControl": bool(getattr(car_params, "openpilotLongitudinalControl", False))'
  '"maxLateralAccel": _safe_float(getattr(car_params, "maxLateralAccel", COMMAVIEW_DEFAULT_MAX_LAT_ACCEL))'
  '"faceOrientation": _float_list(getattr(driver_state.leftDriverData, "faceOrientation", []))'
  '"deviceType": _safe_str(device_state.deviceType)'
  '"sensor": _safe_str(getattr(road_camera_state, "sensor", ""))'
  '"startedFrame": _safe_int(getattr(ui_state, "started_frame", 0))'
  '"startedTime": _safe_float(getattr(ui_state, "started_time", 0.0))'
  '"activeCamera": active_camera'
  '"wideCameraAvailable": wide_camera_available'
  'def set_onroad_camera(self, active_camera: str, wide_camera_available: bool) -> None:'
  '_panda_states_summary(ui_state)'
)

legacy_markers=(
  'def _control_payload(self, ui_state) -> dict:'
  'def _scene_payload(self, ui_state) -> dict:'
  'def _status_payload(self, ui_state) -> dict:'
  'COMMAVIEW_CONTROL_SERVICE_INDEX'
  'COMMAVIEW_SCENE_SERVICE_INDEX'
  'COMMAVIEW_STATUS_SERVICE_INDEX'
  'commaViewControl'
  'commaViewScene'
  'commaViewStatus'
)

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

  [ -f "$HELPER_PATH" ] && helper_present=true || true
  check_fixed 'from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR' "$UI_STATE_PATH" && ui_state_hook_present=true || true
  check_fixed "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$HELPER_PATH" && runtime_flavor_constant_present=true || true
  check_fixed 'COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"' "$HELPER_PATH" && socket_path_present=true || true
  check_fixed 'os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT' "$HELPER_PATH" && socket_env_present=true || true
  check_fixed 'COMMAVIEW_FRAME_VERSION = 1' "$HELPER_PATH" && frame_version_present=true || true
  check_fixed 'socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)' "$HELPER_PATH" && unix_socket_present=true || true
  check_fixed 'struct.pack(">I", len(frame)) + frame' "$HELPER_PATH" && framing_present=true || true
  check_fixed 'json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")' "$HELPER_PATH" && compact_json_present=true || true
  check_fixed 'self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)' "$UI_STATE_PATH" && exporter_install_present=true || true
  check_fixed 'self._commaview_exporter.publish(self)' "$UI_STATE_PATH" && exporter_publish_present=true || true
  if check_fixed 'self._update_commaview_camera_export()' "$AUGMENTED_ROAD_PATH" && \
     check_fixed 'def _update_commaview_camera_export(self):' "$AUGMENTED_ROAD_PATH" && \
     check_fixed 'active_camera="wideRoad" if self.stream_type == WIDE_CAM else "road"' "$AUGMENTED_ROAD_PATH"; then
    onroad_camera_relay_present=true
  fi

  payload_helpers_present=true
  for marker in "${payload_markers[@]}"; do
    if ! check_fixed "$marker" "$HELPER_PATH"; then
      payload_helpers_present=false
      break
    fi
  done

  publish_paths_present=true
  service_marker_count=0
  for const_name in "${service_consts[@]}"; do
    if check_fixed "self._send_json(${const_name}, self._" "$HELPER_PATH"; then
      service_marker_count=$((service_marker_count + 1))
    else
      publish_paths_present=false
    fi
  done

  risk_fields_present=true
  for marker in "${risk_markers[@]}"; do
    if ! check_fixed "$marker" "$HELPER_PATH"; then
      risk_fields_present=false
      break
    fi
  done

  legacy_bucket_markers_absent=true
  for marker in "${legacy_markers[@]}"; do
    if check_fixed "$marker" "$HELPER_PATH"; then
      legacy_bucket_markers_absent=false
      break
    fi
  done

  if $helper_present && $ui_state_hook_present && $runtime_flavor_constant_present && \
     $socket_path_present && $socket_env_present && $frame_version_present && $unix_socket_present && \
     $framing_present && $compact_json_present && $payload_helpers_present && $publish_paths_present && \
     $risk_fields_present && $legacy_bucket_markers_absent && $exporter_install_present && \
     $exporter_publish_present && $onroad_camera_relay_present; then
    state="patch-verified"
    reason="upstream-organized socket UI export markers verified; runtime telemetry not proven"
    patch_verified=true
    repair_needed=false
  else
    state="repair-needed"
    reason="upstream-organized socket UI export markers missing from upstream UI tree"
  fi
fi

json=$(printf '{"healthy":%s,"patchVerified":%s,"statusScope":"%s","repairNeeded":%s,"state":"%s","reason":"%s","flavor":"%s","opRoot":"%s","patch":"%s","patchFingerprint":"%s","helperPresent":%s,"uiStateHookPresent":%s,"runtimeFlavorConstantPresent":%s,"socketPathPresent":%s,"socketEnvPresent":%s,"frameVersionPresent":%s,"unixSocketPresent":%s,"framingPresent":%s,"compactJsonPresent":%s,"payloadHelpersPresent":%s,"publishPathsPresent":%s,"riskFieldsPresent":%s,"legacyBucketMarkersAbsent":%s,"exporterInstallPresent":%s,"exporterPublishPresent":%s,"onroadCameraRelayPresent":%s,"serviceMarkerCount":%s}' "$healthy" "$patch_verified" "$status_scope" "$repair_needed" "$state" "$reason" "$flavor" "$OP_ROOT" "$patch" "$fingerprint" "$helper_present" "$ui_state_hook_present" "$runtime_flavor_constant_present" "$socket_path_present" "$socket_env_present" "$frame_version_present" "$unix_socket_present" "$framing_present" "$compact_json_present" "$payload_helpers_present" "$publish_paths_present" "$risk_fields_present" "$legacy_bucket_markers_absent" "$exporter_install_present" "$exporter_publish_present" "$onroad_camera_relay_present" "$service_marker_count")
printf '%s\n' "$json" > "$STATE_JSON"
if [ -n "$fingerprint" ]; then
  printf 'ONROAD_UI_EXPORT_FLAVOR=%s\nONROAD_UI_EXPORT_PATCH_SHA=%s\nONROAD_UI_EXPORT_OP_ROOT=%s\n' "$flavor" "$fingerprint" "$OP_ROOT" > "$STATE_ENV"
fi
printf '%s\n' "$json"
if $patch_verified; then exit 0; fi
exit 1
