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

  grep -Fq 'commaViewControl' "$patch" || fail "$patch missing control service markers"
  grep -Fq 'commaViewScene' "$patch" || fail "$patch missing scene service markers"
  grep -Fq 'commaViewStatus' "$patch" || fail "$patch missing status service markers"
  grep -Fq 'commaview.capnp' "$patch" || fail "$patch missing dedicated commaview schema wiring"
  grep -Fq 'latActive @14 :Bool;' "$patch" || fail "$patch missing latActive field"
  grep -Fq 'longActive @15 :Bool;' "$patch" || fail "$patch missing longActive field"
  grep -Fq 'experimentalMode @18 :Bool;' "$patch" || fail "$patch missing experimentalMode field"
  grep -Fq 'runtimeFlavor @19 :Text;' "$patch" || fail "$patch missing runtimeFlavor field"
  grep -Fq 'enum CommaViewStatusMode {' "$patch" || fail "$patch missing status mode enum"
  grep -Fq 'enum CommaViewSpeedLimitPreActiveIcon {' "$patch" || fail "$patch missing speed-limit icon enum"
  grep -Fq 'none @0;' "$patch" || fail "$patch missing speed-limit icon none"
  grep -Fq 'up @1;' "$patch" || fail "$patch missing speed-limit icon up"
  grep -Fq 'down @2;' "$patch" || fail "$patch missing speed-limit icon down"
  grep -Fq 'unknown @0;' "$patch" || fail "$patch missing unknown status mode"
  grep -Fq 'disengaged @1;' "$patch" || fail "$patch missing disengaged status mode"
  grep -Fq 'engaged @2;' "$patch" || fail "$patch missing engaged status mode"
  grep -Fq 'override @3;' "$patch" || fail "$patch missing override status mode"
  grep -Fq 'latOnly @4;' "$patch" || fail "$patch missing latOnly status mode"
  grep -Fq 'longOnly @5;' "$patch" || fail "$patch missing longOnly status mode"
  grep -Fq 'statusMode @20 :CommaViewStatusMode;' "$patch" || fail "$patch missing statusMode field"
  grep -Fq 'speedLimitPreActive @21 :Bool;' "$patch" || fail "$patch missing speedLimitPreActive field"
  grep -Fq 'speedLimitPreActiveIcon @22 :CommaViewSpeedLimitPreActiveIcon;' "$patch" || fail "$patch missing speedLimitPreActiveIcon field"
  grep -Fq 'blindspotIndicatorsEnabled @23 :Bool;' "$patch" || fail "$patch missing blindspotIndicatorsEnabled field"
  grep -Fq 'rainbowPathEnabled @24 :Bool;' "$patch" || fail "$patch missing rainbowPathEnabled field"
  grep -Fq 'scene.frameId = int(model.frameId)' "$patch" || fail "$patch missing scene frameId export"
  grep -Fq 'scene.frameDropPerc = float(model.frameDropPerc)' "$patch" || fail "$patch missing scene frameDropPerc export"
  grep -Fq 'scene.timestampEof = int(model.timestampEof)' "$patch" || fail "$patch missing scene timestamp export"
  grep -Fq 'scene.calibration.calPerc = int(live_calibration.calPerc)' "$patch" || fail "$patch missing scene calibration export"
  grep -Fq 'enum CommaViewCameraSource {' "$patch" || fail "$patch missing camera source enum"
  grep -Fq 'cameraIntrinsics @4 :List(Float32);' "$patch" || fail "$patch missing camera intrinsics field"
  grep -Fq 'cameraOffset @5 :List(Float32);' "$patch" || fail "$patch missing camera offset field"
  grep -Fq 'cameraRpyOffset @6 :List(Float32);' "$patch" || fail "$patch missing camera rpy offset field"
  grep -Fq 'activeCamera @12 :CommaViewCameraSource;' "$patch" || fail "$patch missing active camera field"
  grep -Fq 'self._commaview_active_camera = "road"' "$patch" || fail "$patch missing camera hysteresis state"
  grep -Fq 'active_camera, device_camera = self._resolve_commaview_camera()' "$patch" || fail "$patch missing camera resolution helper call"
  grep -Fq 'scene.activeCamera = active_camera' "$patch" || fail "$patch missing active camera export"
  grep -Fq 'scene.calibration.cameraIntrinsics = [' "$patch" || fail "$patch missing camera intrinsics export"
  grep -Fq 'scene.calibration.cameraOffset = [0.0, float(self._commaview_camera_offset()), 0.0]' "$patch" || fail "$patch missing camera offset export"
  grep -Fq 'scene.calibration.cameraRpyOffset = [float(v) for v in live_calibration.wideFromDeviceEuler]' "$patch" || fail "$patch missing wide camera rpy offset export"
  grep -Fq "COMMAVIEW_RUNTIME_FLAVOR = \"$expected_runtime_flavor\"" "$patch" || fail "$patch missing runtime flavor constant"
  grep -Fq 'COMMAVIEW_RUNTIME_FLAVOR_UNKNOWN = "UNKNOWN"' "$patch" || fail "$patch missing runtime flavor unknown fallback"
  grep -Fq 'control.exportVersion = 4' "$patch" || fail "$patch missing control export version bump"
  grep -Fq 'scene.exportVersion = 2' "$patch" || fail "$patch missing scene export version pin"
  grep -Fq 'if self.sm.recv_frame["carControl"] >= self.started_frame:' "$patch" || fail "$patch missing carControl recv guard"
  grep -Fq 'control.latActive = bool(car_control.latActive)' "$patch" || fail "$patch missing latActive export"
  grep -Fq 'control.longActive = bool(car_control.longActive)' "$patch" || fail "$patch missing longActive export"
  grep -Fq 'status.engageable = bool(selfdrive_state.engageable)' "$patch" || fail "$patch missing status engageable export"
  grep -Fq 'status.alertText1 = str(selfdrive_state.alertText1)' "$patch" || fail "$patch missing status alertText1 export"
  grep -Fq 'status.alertType = str(selfdrive_state.alertType)' "$patch" || fail "$patch missing status alertType export"
  grep -Fq 'status.isDistracted = bool(driver_monitoring.isDistracted)' "$patch" || fail "$patch missing DM distraction export"
  grep -Fq 'status.poseYawValidCount = int(driver_monitoring.poseYawValidCount)' "$patch" || fail "$patch missing DM yaw count export"
  grep -Fq 'status.isLowStd = bool(driver_monitoring.isLowStd)' "$patch" || fail "$patch missing DM low-std export"
  grep -Fq 'status.experimentalMode = bool(selfdrive_state.experimentalMode)' "$patch" || fail "$patch missing experimentalMode export"
  grep -Fq 'def _commaview_status_mode_name(status) -> str:' "$patch" || fail "$patch missing status mode helper"
  grep -Fq '"disengaged": "disengaged"' "$patch" || fail "$patch missing disengaged status map"
  grep -Fq '"engaged": "engaged"' "$patch" || fail "$patch missing engaged status map"
  grep -Fq '"override": "override"' "$patch" || fail "$patch missing override status map"
  grep -Fq '"lat_only": "latOnly"' "$patch" || fail "$patch missing latOnly status map"
  grep -Fq '"long_only": "longOnly"' "$patch" || fail "$patch missing longOnly status map"
  grep -Fq 'status.exportVersion = 6' "$patch" || fail "$patch missing status export version bump"
  grep -Fq 'status.runtimeFlavor = COMMAVIEW_RUNTIME_FLAVOR if COMMAVIEW_RUNTIME_FLAVOR in ("OPENPILOT", "SUNNYPILOT") else COMMAVIEW_RUNTIME_FLAVOR_UNKNOWN' "$patch" || fail "$patch missing runtime flavor export"
  grep -Fq 'status.statusMode = "disengaged" if not self.started else "unknown"' "$patch" || fail "$patch missing honest default status mode export"
  grep -Fq 'status.speedLimitPreActive = False' "$patch" || fail "$patch missing default speed-limit pre-active export"
  grep -Fq 'status.speedLimitPreActiveIcon = "none"' "$patch" || fail "$patch missing default speed-limit icon export"
  grep -Fq 'status.blindspotIndicatorsEnabled = bool(self.params.get_bool("BlindSpot"))' "$patch" || fail "$patch missing BlindSpot param export"
  grep -Fq 'status.rainbowPathEnabled = bool(self.params.get_bool("RainbowMode"))' "$patch" || fail "$patch missing RainbowMode param export"
  grep -Fq 'status.statusMode = self._commaview_status_mode_name(self.status)' "$patch" || fail "$patch missing normalized status mode export"
  if [[ "$patch" == *'/sunnypilot/'* ]]; then
    grep -Fq 'from cereal import messaging, car, log, custom' "$patch" || fail "$patch missing sunnypilot custom import"
    grep -Fq 'def _commaview_speed_limit_pre_active_icon(self) -> str:' "$patch" || fail "$patch missing speed-limit icon helper"
    grep -Fq 'status.speedLimitPreActive = speed_limit_assist.state == custom.LongitudinalPlanSP.SpeedLimit.AssistState.preActive' "$patch" || fail "$patch missing speed-limit pre-active export"
    grep -Fq 'status.speedLimitPreActiveIcon = self._commaview_speed_limit_pre_active_icon()' "$patch" || fail "$patch missing speed-limit icon publisher"
  fi
done

echo "PASS: direct v2 UI export patch contract present"
