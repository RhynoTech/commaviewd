#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OPENPILOT_PATCH="$REPO_ROOT/comma4/patches/openpilot/0001-commaview-ui-export-v2.patch"
SUNNYPILOT_PATCH="$REPO_ROOT/comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch"

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

for patch in "$OPENPILOT_PATCH" "$SUNNYPILOT_PATCH"; do
  [[ -f "$patch" ]] || fail "missing patch $patch"
  expected_runtime_flavor='OPENPILOT'
  if [[ "$patch" == *'/sunnypilot/'* ]]; then
    expected_runtime_flavor='SUNNYPILOT'
  fi

  grep -Fq 'diff --git a/selfdrive/ui/commaview_export.py b/selfdrive/ui/commaview_export.py' "$patch" || fail "$patch missing helper file diff"
  grep -Fq 'from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR' "$patch" || fail "$patch missing ui_state direct exporter import"
  grep -Fq "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$patch" || fail "$patch missing runtime flavor constant"
  grep -Fq 'COMMAVIEW_RUNTIME_FLAVOR_UNKNOWN = "UNKNOWN"' "$patch" || fail "$patch missing runtime flavor fallback"
  grep -Fq 'COMMAVIEW_FRAME_VERSION = 1' "$patch" || fail "$patch missing frame version"
  grep -Fq 'COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"' "$patch" || fail "$patch missing default socket path"
  grep -Fq 'os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT' "$patch" || fail "$patch missing socket env override"
  grep -Fq 'socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)' "$patch" || fail "$patch missing unix socket client"
  grep -Fq 'struct.pack(">I", len(frame)) + frame' "$patch" || fail "$patch missing length-prefixed framing"
  grep -Fq 'json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")' "$patch" || fail "$patch missing compact json payload encoding"
  grep -Fq 'def _control_payload(self, ui_state) -> dict:' "$patch" || fail "$patch missing control payload helper"
  grep -Fq 'def _scene_payload(self, ui_state) -> dict:' "$patch" || fail "$patch missing scene payload helper"
  grep -Fq 'def _status_payload(self, ui_state) -> dict:' "$patch" || fail "$patch missing status payload helper"
  grep -Fq 'self._send_json(COMMAVIEW_CONTROL_SERVICE_INDEX, self._control_payload(ui_state))' "$patch" || fail "$patch missing control publish path"
  grep -Fq 'self._send_json(COMMAVIEW_SCENE_SERVICE_INDEX, self._scene_payload(ui_state))' "$patch" || fail "$patch missing scene publish path"
  grep -Fq 'self._send_json(COMMAVIEW_STATUS_SERVICE_INDEX, self._status_payload(ui_state))' "$patch" || fail "$patch missing status publish path"
  grep -Fq '"cruiseSetSpeedMps":' "$patch" || fail "$patch missing cruiseSetSpeedMps export"
  grep -Fq '"latActive": False' "$patch" || fail "$patch missing latActive default"
  grep -Fq '"longActive": False' "$patch" || fail "$patch missing longActive default"
  grep -Fq '"torqueBarValue": 0.0' "$patch" || fail "$patch missing torqueBarValue default"
  grep -Fq '_torque_bar_value(ui_state)' "$patch" || fail "$patch missing torqueBarValue mapping"
  grep -Fq '"driverMonitoring": {' "$patch" || fail "$patch missing nested driverMonitoring object"
  grep -Fq '"driverMonitoringActive":' "$patch" || fail "$patch missing driverMonitoringActive export"
  grep -Fq '"activeCamera": active_camera' "$patch" || fail "$patch missing active camera export"
  grep -Fq '"disengagePredictions": {' "$patch" || fail "$patch missing disengage prediction export object"
  grep -Fq '"brakeDisengageProbs": []' "$patch" || fail "$patch missing brake disengage prediction default export"
  grep -Fq '"steerOverrideProbs": []' "$patch" || fail "$patch missing steer override prediction default export"
  grep -Fq '"accelerationX": []' "$patch" || fail "$patch missing accelerationX default export"
  grep -Fq '"cameraIntrinsics": []' "$patch" || fail "$patch missing calibration intrinsics export"
  grep -Fq '"cameraOffset": [0.0, _commaview_camera_offset(ui_state.params), 0.0]' "$patch" || fail "$patch missing camera offset export"
  grep -Fq '_float_list(getattr(model.meta.disengagePredictions, "brakeDisengageProbs", []))' "$patch" || fail "$patch missing brake disengage prediction mapping"
  grep -Fq '_float_list(getattr(model.meta.disengagePredictions, "steerOverrideProbs", []))' "$patch" || fail "$patch missing steer override prediction mapping"
  grep -Fq '"accelerationX": _float_list(getattr(getattr(model, "acceleration", None), "x", []))' "$patch" || fail "$patch missing accelerationX mapping"
  grep -Fq '"runtimeFlavor": self._flavor' "$patch" || fail "$patch missing runtime flavor export"
  grep -Fq '"statusMode": _status_mode_name(ui_state.status)' "$patch" || fail "$patch missing normalized status mode export"
  grep -Fq '"speedLimitPreActive": False' "$patch" || fail "$patch missing default speed-limit pre-active export"
  grep -Fq '"speedLimitPreActiveIcon": "none"' "$patch" || fail "$patch missing default speed-limit icon export"
  grep -Fq '"blindspotIndicatorsEnabled": bool(ui_state.params.get_bool("BlindSpot"))' "$patch" || fail "$patch missing BlindSpot export"
  grep -Fq '"rainbowPathEnabled": bool(ui_state.params.get_bool("RainbowMode"))' "$patch" || fail "$patch missing RainbowMode export"
  grep -Fq '"torqueBarEnabled":' "$patch" || fail "$patch missing torqueBarEnabled export"
  grep -Fq '"alwaysOnDm": bool(ui_state.always_on_dm)' "$patch" || fail "$patch missing AlwaysOnDM export"
  grep -Fq '"allowThrottle": _allow_throttle(ui_state)' "$patch" || fail "$patch missing allowThrottle export"
  grep -Fq 'self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)' "$patch" || fail "$patch missing exporter installation"
  grep -Fq 'self._commaview_exporter.publish(self)' "$patch" || fail "$patch missing exporter publish call"
  grep -Fq 'cloudlog.exception("commaview ui export publish failed")' "$patch" || fail "$patch missing exporter publish guardrail"

  grep -Fq 'from opendbc.car import ACCELERATION_DUE_TO_GRAVITY' "$patch" || fail "$patch missing torque accel helper import"
  grep -Fq 'def _torque_bar_value(ui_state) -> float:' "$patch" || fail "$patch missing torque bar helper"
  grep -Fq 'def _allow_throttle(ui_state) -> bool:' "$patch" || fail "$patch missing allowThrottle helper"

  if [[ "$patch" == *'/sunnypilot/'* ]]; then
    grep -Fq 'from cereal import custom' "$patch" || fail "$patch missing sunnypilot custom import"
    grep -Fq 'def _speed_limit_pre_active_icon(self, ui_state) -> str:' "$patch" || fail "$patch missing speed-limit icon helper"
    grep -Fq 'custom.LongitudinalPlanSP.SpeedLimit.AssistState.preActive' "$patch" || fail "$patch missing sunnypilot preActive marker"
    grep -Fq '"torqueBarEnabled": bool(ui_state.params.get_bool("TorqueBar"))' "$patch" || fail "$patch missing sunnypilot TorqueBar export"
  fi
done

echo "PASS: socket UI export patch contract present"
