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
  grep -Fq 'commaViewControl' "$patch" || fail "$patch missing control service markers"
  grep -Fq 'commaViewScene' "$patch" || fail "$patch missing scene service markers"
  grep -Fq 'commaViewStatus' "$patch" || fail "$patch missing status service markers"
  grep -Fq 'commaview.capnp' "$patch" || fail "$patch missing dedicated commaview schema wiring"
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
  grep -Fq 'status.engageable = bool(selfdrive_state.engageable)' "$patch" || fail "$patch missing status engageable export"
  grep -Fq 'status.alertText1 = str(selfdrive_state.alertText1)' "$patch" || fail "$patch missing status alertText1 export"
  grep -Fq 'status.alertType = str(selfdrive_state.alertType)' "$patch" || fail "$patch missing status alertType export"
  grep -Fq 'status.isDistracted = bool(driver_monitoring.isDistracted)' "$patch" || fail "$patch missing DM distraction export"
  grep -Fq 'status.poseYawValidCount = int(driver_monitoring.poseYawValidCount)' "$patch" || fail "$patch missing DM yaw count export"
  grep -Fq 'status.isLowStd = bool(driver_monitoring.isLowStd)' "$patch" || fail "$patch missing DM low-std export"
done

echo "PASS: direct v2 UI export patch contract present"
