#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OPENPILOT_PATCH="$REPO_ROOT/comma4/patches/openpilot/0001-commaview-ui-export-v2.patch"
SUNNYPILOT_PATCH="$REPO_ROOT/comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch"
CACHE_ROOT="${COMMAVIEWD_CANARY_CACHE_ROOT:-$HOME/.cache/ci-ref-checkouts}"
OPENPILOT_REPO="${COMMAVIEWD_OPENPILOT_REPO:-https://github.com/commaai/openpilot.git}"
SUNNYPILOT_REPO="${COMMAVIEWD_SUNNYPILOT_REPO:-https://github.com/sunnyhaibin/sunnypilot.git}"

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

run_ref() {
  local label="$1"
  local repo="$2"
  local ref="$3"
  local checkout="$4"
  local patch="$5"
  local expected_runtime_flavor="$6"

  echo "=== ${label} ==="
  mkdir -p "$(dirname "$checkout")"
  if [[ -e "$checkout/.git" ]]; then
    git -C "$checkout" fetch --depth 1 origin "$ref"
    git -C "$checkout" checkout -q FETCH_HEAD
  else
    git clone --depth 1 --branch "$ref" "$repo" "$checkout"
  fi

  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq
  git -C "$checkout" apply --recount --check "$patch" || fail "patch does not apply cleanly for ${label}"
  git -C "$checkout" apply --recount "$patch"

  helper_path="$checkout/selfdrive/ui/commaview_export.py"
  ui_state_path="$checkout/selfdrive/ui/ui_state.py"

  grep -Fq 'from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR' "$ui_state_path" || fail "ui_state exporter import missing for ${label}"
  grep -Fq 'self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)' "$ui_state_path" || fail "ui_state exporter install missing for ${label}"
  grep -Fq 'self._commaview_exporter.publish(self)' "$ui_state_path" || fail "ui_state exporter publish missing for ${label}"
  grep -Fq 'cloudlog.exception("commaview ui export publish failed")' "$ui_state_path" || fail "ui_state exporter guardrail missing for ${label}"
  grep -Fq "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$helper_path" || fail "runtime flavor constant missing for ${label}"
  grep -Fq 'COMMAVIEW_FRAME_VERSION = 1' "$helper_path" || fail "frame version missing for ${label}"
  grep -Fq 'COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"' "$helper_path" || fail "socket path missing for ${label}"
  grep -Fq 'os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT' "$helper_path" || fail "socket env override missing for ${label}"
  grep -Fq 'socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)' "$helper_path" || fail "unix socket client missing for ${label}"
  grep -Fq 'struct.pack(">I", len(frame)) + frame' "$helper_path" || fail "frame packing missing for ${label}"
  grep -Fq 'json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")' "$helper_path" || fail "compact json encoding missing for ${label}"
  grep -Fq 'from opendbc.car import ACCELERATION_DUE_TO_GRAVITY' "$helper_path" || fail "torque accel helper import missing for ${label}"
  grep -Fq 'def _torque_bar_value(ui_state) -> float:' "$helper_path" || fail "torque bar helper missing for ${label}"

  for marker in "${payload_markers[@]}"; do
    grep -Fq "$marker" "$helper_path" || fail "payload helper missing for ${label}: $marker"
  done

  for const_name in "${service_consts[@]}"; do
    grep -Fq "self._send_json(${const_name}, self._" "$helper_path" || fail "publish path missing for ${label}: $const_name"
  done

  for marker in "${risk_markers[@]}"; do
    grep -Fq "$marker" "$helper_path" || fail "risk-field marker missing for ${label}: $marker"
  done

  for marker in "${legacy_markers[@]}"; do
    ! grep -Fq "$marker" "$helper_path" || fail "legacy marker still present for ${label}: $marker"
  done

  printf '%s\n' '{"healthy":false,"patchVerified":true,"statusScope":"patch-installation","repairNeeded":false,"state":"patch-verified","reason":"upstream-organized socket ui export verified on real canary ref"}'

  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq
}

run_ref 'openpilot release-mici-staging' "$OPENPILOT_REPO" 'release-mici-staging' "$CACHE_ROOT/openpilot-release-mici-staging" "$OPENPILOT_PATCH" 'OPENPILOT'
run_ref 'openpilot nightly' "$OPENPILOT_REPO" 'nightly' "$CACHE_ROOT/openpilot-nightly" "$OPENPILOT_PATCH" 'OPENPILOT'
run_ref 'sunnypilot staging' "$SUNNYPILOT_REPO" 'staging' "$CACHE_ROOT/sunnypilot-staging" "$SUNNYPILOT_PATCH" 'SUNNYPILOT'
run_ref 'sunnypilot dev' "$SUNNYPILOT_REPO" 'dev' "$CACHE_ROOT/sunnypilot-dev" "$SUNNYPILOT_PATCH" 'SUNNYPILOT'

echo 'PASS: upstream-organized socket UI export patch applies and verifies on real canary refs'
