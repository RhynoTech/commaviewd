#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GUARD="$ROOT/scripts/upstream-interface-guard.sh"

tmpdir="$(mktemp -d)"
cleanup() { python3 - "$tmpdir" <<'PY'
import shutil
import sys
shutil.rmtree(sys.argv[1], ignore_errors=True)
PY
}
trap cleanup EXIT

op_root="$tmpdir/openpilot-src"
manifest="$tmpdir/manifest.json"
source_root="$op_root/cereal"
mkdir -p "$source_root"

cat > "$source_root/services.py" <<'PY'
roadEncodeData = None
wideRoadEncodeData = None
driverEncodeData = None
livestreamRoadEncodeData = None
livestreamWideRoadEncodeData = None
livestreamDriverEncodeData = None
carState = None
selfdriveState = None
deviceState = None
liveCalibration = None
radarState = None
modelV2 = None
controlsState = None
onroadEvents = None
driverMonitoringState = None
driverStateV2 = None
carOutput = None
carControl = None
liveParameters = None
longitudinalPlan = None
carParams = None
roadCameraState = None
pandaStates = None
wideRoadCameraState = None
PY

cat > "$source_root/log.capnp" <<'CAPNP'
roadEncodeData
wideRoadEncodeData
driverEncodeData
livestreamRoadEncodeData
livestreamWideRoadEncodeData
livestreamDriverEncodeData
carState
selfdriveState
deviceState
liveCalibration
radarState
modelV2
controlsState
onroadEvents
driverMonitoringState
driverStateV2
carOutput
carControl
liveParameters
longitudinalPlan
carParams
roadCameraState
pandaStates
wideRoadCameraState
alertText1
alertText2
alertType
laneLineProbs
laneLineStds
roadEdgeStds
leadsV3
rpyCalib
calStatus
calPerc
CAPNP

git -C "$op_root" init -q
git -C "$op_root" remote add origin https://github.com/commaai/openpilot.git

output="$(OP_ROOT="$op_root" "$GUARD" --manifest "$manifest" 2>&1)" || {
  printf '%s\n' "$output" >&2
  echo "FAIL: transformer-era upstream guard should not require static direct-v2 patch applicability" >&2
  exit 1
}

printf '%s\n' "$output" | grep -Fq 'PASS: upstream interface guard' || {
  printf '%s\n' "$output" >&2
  echo "FAIL: guard did not report success" >&2
  exit 1
}

python3 - "$manifest" <<'PY'
import json
import sys
manifest = json.loads(open(sys.argv[1]).read())
checks = manifest.get("checks", {})
if checks.get("onroadUiExportMethod") != "transformer":
    raise SystemExit(f"missing transformer method in manifest: {checks}")
if "directV2PatchFlavor" in checks:
    raise SystemExit(f"stale static patch manifest key present: {checks}")
expected_services = 24
if checks.get("requiredServices", 0) < expected_services:
    raise SystemExit(f"guard should cover all exporter services; expected at least {expected_services}, got {checks.get('requiredServices')}: {checks}")
PY

echo "PASS: upstream interface guard validates transformer prerequisites"

nested_op_root="$tmpdir/openpilot-nested"
nested_manifest="$tmpdir/nested-manifest.json"
mkdir -p "$nested_op_root/openpilot"
cp -a "$op_root/cereal" "$nested_op_root/openpilot/cereal"
git -C "$nested_op_root" init -q
git -C "$nested_op_root" remote add origin https://github.com/commaai/openpilot.git

nested_output="$(OP_ROOT="$nested_op_root" "$GUARD" --manifest "$nested_manifest" 2>&1)" || {
  printf '%s\n' "$nested_output" >&2
  echo "FAIL: upstream guard should support nested openpilot package roots" >&2
  exit 1
}

printf '%s\n' "$nested_output" | grep -Fq 'PASS: upstream interface guard' || {
  printf '%s\n' "$nested_output" >&2
  echo "FAIL: nested guard did not report success" >&2
  exit 1
}

python3 - "$nested_manifest" "$nested_op_root/openpilot" <<'PY'
import json
import sys
manifest = json.loads(open(sys.argv[1]).read())
if manifest.get("opSourceRoot") != sys.argv[2]:
    raise SystemExit(f"nested manifest should record opSourceRoot: {manifest}")
PY

echo "PASS: upstream interface guard validates nested openpilot package roots"
