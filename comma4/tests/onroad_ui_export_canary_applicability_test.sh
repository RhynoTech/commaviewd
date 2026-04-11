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

  grep -Fq 'from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR' "$ui_state_path" || fail "ui_state direct exporter import missing for ${label}"
  grep -Fq 'self._commaview_exporter = _CommaViewSocketExporter(COMMAVIEW_RUNTIME_FLAVOR)' "$ui_state_path" || fail "ui_state exporter install missing for ${label}"
  grep -Fq 'self._commaview_exporter.publish(self)' "$ui_state_path" || fail "ui_state exporter publish missing for ${label}"
  grep -Fq "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$helper_path" || fail "runtime flavor constant missing for ${label}"
  grep -Fq 'COMMAVIEW_FRAME_VERSION = 1' "$helper_path" || fail "frame version missing for ${label}"
  grep -Fq 'COMMAVIEW_SOCKET_PATH_DEFAULT = "/data/commaview/run/ui-export.sock"' "$helper_path" || fail "socket path missing for ${label}"
  grep -Fq 'os.environ.get("COMMAVIEWD_UI_EXPORT_SOCKET") or COMMAVIEW_SOCKET_PATH_DEFAULT' "$helper_path" || fail "socket env override missing for ${label}"
  grep -Fq 'socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)' "$helper_path" || fail "unix socket client missing for ${label}"
  grep -Fq 'struct.pack(">I", len(frame)) + frame' "$helper_path" || fail "frame packing missing for ${label}"
  grep -Fq 'def _control_payload(self, ui_state) -> dict:' "$helper_path" || fail "control payload helper missing for ${label}"
  grep -Fq 'def _scene_payload(self, ui_state) -> dict:' "$helper_path" || fail "scene payload helper missing for ${label}"
  grep -Fq 'def _status_payload(self, ui_state) -> dict:' "$helper_path" || fail "status payload helper missing for ${label}"
  grep -Fq 'self._send_json(COMMAVIEW_CONTROL_SERVICE_INDEX, self._control_payload(ui_state))' "$helper_path" || fail "control publish path missing for ${label}"
  grep -Fq 'self._send_json(COMMAVIEW_SCENE_SERVICE_INDEX, self._scene_payload(ui_state))' "$helper_path" || fail "scene publish path missing for ${label}"
  grep -Fq 'self._send_json(COMMAVIEW_STATUS_SERVICE_INDEX, self._status_payload(ui_state))' "$helper_path" || fail "status publish path missing for ${label}"
  grep -Fq '"cruiseSetSpeedMps":' "$helper_path" || fail "cruiseSetSpeedMps export missing for ${label}"
  grep -Fq '"torqueBarValue": 0.0' "$helper_path" || fail "torqueBarValue default missing for ${label}"
  grep -Fq '_torque_bar_value(ui_state)' "$helper_path" || fail "torqueBarValue mapping missing for ${label}"
  grep -Fq '"driverMonitoring": {' "$helper_path" || fail "nested driverMonitoring export missing for ${label}"
  grep -Fq '"activeCamera": active_camera' "$helper_path" || fail "active camera export missing for ${label}"
  grep -Fq '"disengagePredictions": {' "$helper_path" || fail "disengage prediction export missing for ${label}"
  grep -Fq '_float_list(getattr(model.meta.disengagePredictions, "brakeDisengageProbs", []))' "$helper_path" || fail "brake disengage prediction mapping missing for ${label}"
  grep -Fq '_float_list(getattr(model.meta.disengagePredictions, "steerOverrideProbs", []))' "$helper_path" || fail "steer override prediction mapping missing for ${label}"
  grep -Fq '"accelerationX": _float_list(getattr(getattr(model, "acceleration", None), "x", []))' "$helper_path" || fail "accelerationX export missing for ${label}"
  grep -Fq '"runtimeFlavor": self._flavor' "$helper_path" || fail "runtime flavor export missing for ${label}"
  grep -Fq '"statusMode": _status_mode_name(ui_state.status)' "$helper_path" || fail "status mode export missing for ${label}"
  grep -Fq '"torqueBarEnabled":' "$helper_path" || fail "torqueBarEnabled export missing for ${label}"
  grep -Fq '"alwaysOnDm": bool(ui_state.always_on_dm)' "$helper_path" || fail "AlwaysOnDM export missing for ${label}"
  grep -Fq '"allowThrottle": _allow_throttle(ui_state)' "$helper_path" || fail "allowThrottle export missing for ${label}"
  grep -Fq 'from opendbc.car import ACCELERATION_DUE_TO_GRAVITY' "$helper_path" || fail "torque accel helper import missing for ${label}"
  grep -Fq 'def _torque_bar_value(ui_state) -> float:' "$helper_path" || fail "torque bar helper missing for ${label}"
  grep -Fq 'def _allow_throttle(ui_state) -> bool:' "$helper_path" || fail "allowThrottle helper missing for ${label}"
  grep -Fq 'cloudlog.exception("commaview ui export publish failed")' "$ui_state_path" || fail "ui_state exporter guardrail missing for ${label}"

  if [[ "$label" == sunnypilot* ]]; then
    grep -Fq 'from cereal import custom' "$helper_path" || fail "custom import missing for ${label}"
    grep -Fq 'def _speed_limit_pre_active_icon(self, ui_state) -> str:' "$helper_path" || fail "speed-limit icon helper missing for ${label}"
    grep -Fq 'custom.LongitudinalPlanSP.SpeedLimit.AssistState.preActive' "$helper_path" || fail "preActive marker missing for ${label}"
    grep -Fq '"torqueBarEnabled": bool(ui_state.params.get_bool("TorqueBar"))' "$helper_path" || fail "TorqueBar export missing for ${label}"
  fi

  printf '%s\n' '{"healthy":false,"patchVerified":true,"statusScope":"patch-installation","repairNeeded":false,"state":"patch-verified","reason":"socket ui export direct wiring verified on real canary ref"}'

  git -C "$checkout" reset --hard -q HEAD
  git -C "$checkout" clean -fdq
}

run_ref 'openpilot release-mici-staging' "$OPENPILOT_REPO" 'release-mici-staging' "$CACHE_ROOT/openpilot-release-mici-staging" "$OPENPILOT_PATCH" 'OPENPILOT'
run_ref 'openpilot nightly' "$OPENPILOT_REPO" 'nightly' "$CACHE_ROOT/openpilot-nightly" "$OPENPILOT_PATCH" 'OPENPILOT'
run_ref 'sunnypilot staging' "$SUNNYPILOT_REPO" 'staging' "$CACHE_ROOT/sunnypilot-staging" "$SUNNYPILOT_PATCH" 'SUNNYPILOT'
run_ref 'sunnypilot dev' "$SUNNYPILOT_REPO" 'dev' "$CACHE_ROOT/sunnypilot-dev" "$SUNNYPILOT_PATCH" 'SUNNYPILOT'

echo 'PASS: socket UI export patch applies and verifies on real canary refs'
