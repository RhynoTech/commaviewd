#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
SRC_ROOT="$INSTALL_DIR/src"
TRANSFORMER="$INSTALL_DIR/scripts/transform_onroad_ui_export.py"
STATE_JSON="$INSTALL_DIR/run/onroad-ui-export-status.json"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"
HELPER_PATH="$OP_ROOT/selfdrive/ui/commaview_export.py"
UI_STATE_PATH="$OP_ROOT/selfdrive/ui/ui_state.py"
AUGMENTED_ROAD_PATHS=(
  "$OP_ROOT/selfdrive/ui/mici/onroad/augmented_road_view.py"
  "$OP_ROOT/selfdrive/ui/onroad/augmented_road_view.py"
)
AUGMENTED_ROAD_PATH="${AUGMENTED_ROAD_PATHS[0]}"
for candidate in "${AUGMENTED_ROAD_PATHS[@]}"; do
  if [ -f "$candidate" ]; then
    AUGMENTED_ROAD_PATH="$candidate"
    break
  fi
done
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

state_value() {
  local key="$1"
  [ -f "$STATE_ENV" ] || return 1
  sed -n "s/^${key}=//p" "$STATE_ENV" | tail -n 1 | sed 's/^"//; s/"$//'
}

remote_flavor() {
  local remote="$1"
  remote="${remote%.git}"
  remote="${remote%/}"
  case "$remote" in
    *github.com:commaai/openpilot|*github.com/commaai/openpilot) printf '%s\n' openpilot ;;
    *github.com:sunnypilot/sunnypilot|*github.com/sunnypilot/sunnypilot|*github.com:sunnypilot/openpilot|*github.com/sunnypilot/openpilot) printf '%s\n' sunnypilot ;;
    *) return 1 ;;
  esac
}

detect_flavor() {
  local preferred=""
  local remote=""
  local state_flavor=""
  local state_op_root=""

  if git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
    if [ -n "$remote" ]; then
      preferred="$(remote_flavor "$remote")" || return 1
    fi
  fi

  if [ -z "$preferred" ]; then
    state_flavor="$(state_value ONROAD_UI_EXPORT_FLAVOR || true)"
    state_op_root="$(state_value ONROAD_UI_EXPORT_OP_ROOT || true)"
    if [ "$state_op_root" = "$OP_ROOT" ] && { [ "$state_flavor" = "openpilot" ] || [ "$state_flavor" = "sunnypilot" ]; }; then
      preferred="$state_flavor"
    fi
  fi

  if [ -n "$preferred" ] && [ -f "$SRC_ROOT/commaview_export.${preferred}.py" ]; then
    printf '%s\n' "$preferred"
    return 0
  fi
  return 1
}

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
  COMMAVIEW_WIDE_ROAD_CAMERA_STATE_SERVICE_INDEX
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
  'def set_onroad_projection('
  'def _wide_road_camera_state_payload(self, ui_state) -> dict:'
)

risk_markers=(
  '"alertHudVisual": 0'
  '"allowThrottle": False'
  '"openpilotLongitudinalControl": bool(getattr(car_params, "openpilotLongitudinalControl", False))'
  '"maxLateralAccel": _safe_float(getattr(car_params, "maxLateralAccel", COMMAVIEW_DEFAULT_MAX_LAT_ACCEL))'
  '"carFingerprint": _safe_str(getattr(car_params, "carFingerprint", ""))'
  '"carName": _safe_str(getattr(car_params, "carName", ""))'
  '"carVin": _safe_str(getattr(car_params, "carVin", ""))'
  '"faceOrientation": _float_list(getattr(driver_data, "faceOrientation", []))'
  '"facePosition": _float_list(getattr(driver_data, "facePosition", []))'
  '"faceOrientationStd": _float_list(getattr(driver_data, "faceOrientationStd", []))'
  '"facePositionStd": _float_list(getattr(driver_data, "facePositionStd", []))'
  '"wheelOnRightProb": _safe_float(getattr(driver_state, "wheelOnRightProb", 0.0))'
  '"deviceType": _safe_str(device_state.deviceType)'
  '"sensor": _safe_str(getattr(road_camera_state, "sensor", ""))'
  '"sensor": _safe_str(getattr(wide_camera_state, "sensor", ""))'
  '"startedFrame": _safe_int(getattr(ui_state, "started_frame", 0))'
  '"startedTime": _safe_float(getattr(ui_state, "started_time", 0.0))'
  '"activeCamera": active_camera'
  '"wideCameraAvailable": wide_camera_available'
  'def set_onroad_camera(self, active_camera: str, wide_camera_available: bool) -> None:'
  'def set_onroad_projection('
  'self._latest_onroad_projection = {'
  '"modelTransform": _matrix3_list(model_transform)'
  '"runtimeFlavor": self._flavor'
  '"enabled": bool(getattr(controls_state, "enabled", False))'
  '"active": bool(getattr(controls_state, "active", False))'
  '"engageable": bool(getattr(controls_state, "engageable", False))'
  '"setSpeed": _safe_float(getattr(hud_control, "setSpeed", 0.0))'
  '"speedVisible": bool(getattr(hud_control, "speedVisible", False))'
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

if flavor="$(detect_flavor)"; then
  flavor_detected=1
else
  flavor=""
  flavor_detected=0
fi
template_path="$SRC_ROOT/commaview_export.${flavor}.py"
state="stale"
status_scope="patch-installation"
method="transformer"
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
onroad_projection_present=false
transformer_present=false
template_present=false
fingerprint=""
service_marker_count=0

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  state="missing-repo"; reason="upstream repo not found at $OP_ROOT"
elif [ "$flavor_detected" -ne 1 ]; then
  state="unknown-flavor"; reason="unsupported upstream remote for $OP_ROOT; CommaView currently supports only commaai/openpilot and sunnypilot remotes"
elif [ ! -f "$template_path" ]; then
  state="missing-template"; reason="missing socket UI export helper template for $flavor"
elif [ ! -f "$TRANSFORMER" ]; then
  state="missing-transformer"; reason="missing socket UI export transformer"
else
  transformer_present=true
  template_present=true
  fingerprint="$(sha256sum "$TRANSFORMER" "$template_path" | sha256sum | awk '{print $1}')"
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
  if check_fixed 'self._update_commaview_camera_export()' "$AUGMENTED_ROAD_PATH" &&      check_fixed 'def _update_commaview_camera_export(self):' "$AUGMENTED_ROAD_PATH" &&      check_fixed 'active_camera="wideRoad" if self.stream_type == WIDE_CAM else "road"' "$AUGMENTED_ROAD_PATH"; then
    onroad_camera_relay_present=true
  fi
  if check_fixed 'model_transform = video_transform @ calib_transform' "$AUGMENTED_ROAD_PATH" &&      check_fixed 'exporter.set_onroad_projection(' "$AUGMENTED_ROAD_PATH" &&      check_fixed 'active_camera="wideRoad" if is_wide_camera else "road"' "$AUGMENTED_ROAD_PATH" &&      check_fixed 'video_frame_matrix=self._cached_matrix' "$AUGMENTED_ROAD_PATH" &&      { check_fixed 'camera_offset=getattr(self._model_renderer, "_camera_offset", 0.0)' "$AUGMENTED_ROAD_PATH" || check_fixed 'camera_offset=getattr(self.model_renderer, "_camera_offset", 0.0)' "$AUGMENTED_ROAD_PATH"; } &&      check_fixed 'self._send_json(COMMAVIEW_ONROAD_PROJECTION_SERVICE_INDEX, self._latest_onroad_projection)' "$HELPER_PATH"; then
    onroad_projection_present=true
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

  if $helper_present && $ui_state_hook_present && $runtime_flavor_constant_present &&      $socket_path_present && $socket_env_present && $frame_version_present && $unix_socket_present &&      $framing_present && $compact_json_present && $payload_helpers_present && $publish_paths_present &&      $risk_fields_present && $legacy_bucket_markers_absent && $exporter_install_present &&      $exporter_publish_present && $onroad_camera_relay_present && $onroad_projection_present; then
    state="patch-verified"
    reason="source transformer socket UI export markers verified; runtime telemetry not proven"
    patch_verified=true
    repair_needed=false
  else
    state="repair-needed"
    reason="source transformer socket UI export markers missing from upstream UI tree"
  fi
fi

json="$(
  HEALTHY="$healthy" \
  PATCH_VERIFIED="$patch_verified" \
  METHOD="$method" \
  STATUS_SCOPE="$status_scope" \
  REPAIR_NEEDED="$repair_needed" \
  STATE="$state" \
  REASON="$reason" \
  FLAVOR="$flavor" \
  OP_ROOT_JSON="$OP_ROOT" \
  TRANSFORMER_JSON="$TRANSFORMER" \
  TEMPLATE_PATH_JSON="$template_path" \
  FINGERPRINT="$fingerprint" \
  HELPER_PRESENT="$helper_present" \
  UI_STATE_HOOK_PRESENT="$ui_state_hook_present" \
  RUNTIME_FLAVOR_CONSTANT_PRESENT="$runtime_flavor_constant_present" \
  SOCKET_PATH_PRESENT="$socket_path_present" \
  SOCKET_ENV_PRESENT="$socket_env_present" \
  FRAME_VERSION_PRESENT="$frame_version_present" \
  UNIX_SOCKET_PRESENT="$unix_socket_present" \
  FRAMING_PRESENT="$framing_present" \
  COMPACT_JSON_PRESENT="$compact_json_present" \
  PAYLOAD_HELPERS_PRESENT="$payload_helpers_present" \
  PUBLISH_PATHS_PRESENT="$publish_paths_present" \
  RISK_FIELDS_PRESENT="$risk_fields_present" \
  LEGACY_BUCKET_MARKERS_ABSENT="$legacy_bucket_markers_absent" \
  EXPORTER_INSTALL_PRESENT="$exporter_install_present" \
  EXPORTER_PUBLISH_PRESENT="$exporter_publish_present" \
  ONROAD_CAMERA_RELAY_PRESENT="$onroad_camera_relay_present" \
  ONROAD_PROJECTION_PRESENT="$onroad_projection_present" \
  TRANSFORMER_PRESENT="$transformer_present" \
  TEMPLATE_PRESENT="$template_present" \
  SERVICE_MARKER_COUNT="$service_marker_count" \
  python3 - <<'PYJSON'
import json
import os

def env_bool(name: str) -> bool:
    return os.environ.get(name, "false") == "true"

def env_int(name: str) -> int:
    try:
        return int(os.environ.get(name, "0"))
    except ValueError:
        return 0

payload = {
    "healthy": env_bool("HEALTHY"),
    "patchVerified": env_bool("PATCH_VERIFIED"),
    "method": os.environ.get("METHOD", ""),
    "statusScope": os.environ.get("STATUS_SCOPE", ""),
    "repairNeeded": env_bool("REPAIR_NEEDED"),
    "state": os.environ.get("STATE", ""),
    "reason": os.environ.get("REASON", ""),
    "flavor": os.environ.get("FLAVOR", ""),
    "opRoot": os.environ.get("OP_ROOT_JSON", ""),
    "transformer": os.environ.get("TRANSFORMER_JSON", ""),
    "template": os.environ.get("TEMPLATE_PATH_JSON", ""),
    "transformerFingerprint": os.environ.get("FINGERPRINT", ""),
    "helperPresent": env_bool("HELPER_PRESENT"),
    "uiStateHookPresent": env_bool("UI_STATE_HOOK_PRESENT"),
    "runtimeFlavorConstantPresent": env_bool("RUNTIME_FLAVOR_CONSTANT_PRESENT"),
    "socketPathPresent": env_bool("SOCKET_PATH_PRESENT"),
    "socketEnvPresent": env_bool("SOCKET_ENV_PRESENT"),
    "frameVersionPresent": env_bool("FRAME_VERSION_PRESENT"),
    "unixSocketPresent": env_bool("UNIX_SOCKET_PRESENT"),
    "framingPresent": env_bool("FRAMING_PRESENT"),
    "compactJsonPresent": env_bool("COMPACT_JSON_PRESENT"),
    "payloadHelpersPresent": env_bool("PAYLOAD_HELPERS_PRESENT"),
    "publishPathsPresent": env_bool("PUBLISH_PATHS_PRESENT"),
    "riskFieldsPresent": env_bool("RISK_FIELDS_PRESENT"),
    "legacyBucketMarkersAbsent": env_bool("LEGACY_BUCKET_MARKERS_ABSENT"),
    "exporterInstallPresent": env_bool("EXPORTER_INSTALL_PRESENT"),
    "exporterPublishPresent": env_bool("EXPORTER_PUBLISH_PRESENT"),
    "onroadCameraRelayPresent": env_bool("ONROAD_CAMERA_RELAY_PRESENT"),
    "onroadProjectionPresent": env_bool("ONROAD_PROJECTION_PRESENT"),
    "transformerPresent": env_bool("TRANSFORMER_PRESENT"),
    "templatePresent": env_bool("TEMPLATE_PRESENT"),
    "serviceMarkerCount": env_int("SERVICE_MARKER_COUNT"),
}
print(json.dumps(payload, separators=(",", ":")))
PYJSON
)"
printf '%s
' "$json" > "$STATE_JSON"
if [ -n "$fingerprint" ]; then
  printf 'ONROAD_UI_EXPORT_FLAVOR=%s
ONROAD_UI_EXPORT_METHOD=transformer
ONROAD_UI_EXPORT_TRANSFORMER_SHA=%s
ONROAD_UI_EXPORT_OP_ROOT=%s
' "$flavor" "$fingerprint" "$OP_ROOT" > "$STATE_ENV"
fi
printf '%s
' "$json"
if $patch_verified; then exit 0; fi
exit 1
