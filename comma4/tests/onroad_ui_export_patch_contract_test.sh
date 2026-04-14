#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OPENPILOT_PATCH="$REPO_ROOT/comma4/patches/openpilot/0001-commaview-ui-export-v2.patch"
SUNNYPILOT_PATCH="$REPO_ROOT/comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch"

fail() {
  echo "FAIL: $1" >&2
  exit 1
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
  '"runtimeFlavor": self._flavor'
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
  'COMMAVIEW_RUNTIME_FLAVOR_UNKNOWN = "UNKNOWN"'
  'from cereal import custom'
  'def _speed_limit_pre_active_icon(self, ui_state) -> str:'
)

for patch in "$OPENPILOT_PATCH" "$SUNNYPILOT_PATCH"; do
  [[ -f "$patch" ]] || fail "missing patch $patch"
  expected_runtime_flavor='OPENPILOT'
  if [[ "$patch" == *'/sunnypilot/'* ]]; then
    expected_runtime_flavor='SUNNYPILOT'
  fi

  grep -Fq 'diff --git a/selfdrive/ui/commaview_export.py b/selfdrive/ui/commaview_export.py' "$patch" || fail "$patch missing helper file diff"
  grep -Fq 'from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR' "$patch" || fail "$patch missing ui_state exporter import"
  grep -Fq "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$patch" || fail "$patch missing runtime flavor constant"
  grep -Fq 'COMMAVIEW_FRAME_VERSION = 1' "$patch" || fail "$patch missing frame version"
  grep -Fq 'COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"' "$patch" || fail "$patch missing default socket path"
  grep -Fq 'os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT' "$patch" || fail "$patch missing socket env override"
  grep -Fq 'socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)' "$patch" || fail "$patch missing unix socket client"
  grep -Fq 'struct.pack(">I", len(frame)) + frame' "$patch" || fail "$patch missing framing"
  grep -Fq 'json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")' "$patch" || fail "$patch missing compact json encoding"
  grep -Fq 'self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)' "$patch" || fail "$patch missing exporter installation"
  grep -Fq 'self._commaview_exporter.publish(self)' "$patch" || fail "$patch missing exporter publish call"
  grep -Fq 'cloudlog.exception("commaview ui export publish failed")' "$patch" || fail "$patch missing publish guardrail"
  grep -Fq 'from opendbc.car import ACCELERATION_DUE_TO_GRAVITY' "$patch" || fail "$patch missing torque helper import"
  grep -Fq 'def _torque_bar_value(ui_state) -> float:' "$patch" || fail "$patch missing torque bar helper"

  for marker in "${payload_markers[@]}"; do
    grep -Fq "$marker" "$patch" || fail "$patch missing payload marker: $marker"
  done

  for const_name in "${service_consts[@]}"; do
    grep -Fq "self._send_json(${const_name}, self._" "$patch" || fail "$patch missing publish path for $const_name"
  done

  for marker in "${risk_markers[@]}"; do
    grep -Fq "$marker" "$patch" || fail "$patch missing risk-field marker: $marker"
  done

  for marker in "${legacy_markers[@]}"; do
    ! grep -Fq "$marker" "$patch" || fail "$patch still contains legacy marker: $marker"
  done
done

echo "PASS: upstream-organized socket UI export patch contract present"
