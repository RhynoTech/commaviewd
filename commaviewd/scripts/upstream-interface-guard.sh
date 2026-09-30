#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/path/to/openpilot-src commaviewd/scripts/upstream-interface-guard.sh [--manifest <path>] [--telemetry-only]
Fast-fails when upstream schema/service interfaces drift in ways that can break CommaViewD.
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
DEFAULT_OP_ROOT="$REPO_ROOT/../openpilot-src"
if [[ -d "$DEFAULT_OP_ROOT" ]]; then
  OP_ROOT="${OP_ROOT:-$DEFAULT_OP_ROOT}"
else
  OP_ROOT="${OP_ROOT:-$HOME/openpilot-src}"
fi
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
MANIFEST="$DIST_DIR/upstream-interface-manifest.json"
TELEMETRY_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest) MANIFEST="$2"; shift 2 ;;
    --telemetry-only) TELEMETRY_ONLY=1; shift ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

missing=()

source_root() {
  if [[ -f "$OP_ROOT/cereal/log.capnp" && -f "$OP_ROOT/cereal/services.py" ]]; then
    printf '%s\n' "$OP_ROOT"
    return 0
  fi
  if [[ -f "$OP_ROOT/openpilot/cereal/log.capnp" && -f "$OP_ROOT/openpilot/cereal/services.py" ]]; then
    printf '%s\n' "$OP_ROOT/openpilot"
    return 0
  fi
  printf '%s\n' "$OP_ROOT"
}

OP_SOURCE_ROOT="$(source_root)"

check_file() {
  local p="$1"
  [[ -f "$p" ]] || missing+=("file:$p")
}

has_token() {
  local file="$1"
  local token="$2"
  grep -Eq "(^|[^A-Za-z0-9_])${token}([^A-Za-z0-9_]|$)" "$file"
}

check_token() {
  has_token "$1" "$2" || missing+=("token:$2@$1")
}

required_files=(
  "$OP_SOURCE_ROOT/cereal/log.capnp"
  "$OP_SOURCE_ROOT/cereal/services.py"
)

for f in "${required_files[@]}"; do
  check_file "$f"
done

remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
onroad_ui_export_flavor="openpilot"
if printf '%s' "$remote" | grep -qi 'sunnypilot'; then
  onroad_ui_export_flavor="sunnypilot"
fi

required_transformer_files=(
  "$REPO_ROOT/comma/scripts/transform_onroad_ui_export.py"
  "$REPO_ROOT/comma/scripts/apply_onroad_ui_export_patch.sh"
  "$REPO_ROOT/comma/scripts/verify_onroad_ui_export_patch.sh"
  "$REPO_ROOT/comma/src/commaview_export.$onroad_ui_export_flavor.py"
)

for f in "${required_transformer_files[@]}"; do
  check_file "$f"
done

[[ ${#missing[@]} -eq 0 ]] || {
  printf 'FAIL: upstream interface guard missing current transformer prerequisites:\n' >&2
  printf '  - %s\n' "${missing[@]}" >&2
  exit 1
}

required_services=(
  wideRoadEncodeData
  livestreamWideRoadEncodeData
  carState
  selfdriveState
  deviceState
  radarState
  modelV2
  controlsState
  onroadEvents
  driverMonitoringState
  driverStateV2
  carOutput
  carControl
  longitudinalPlan
  carParams
  pandaStates
  wideRoadCameraState
  gpsLocationExternal
  gpsLocation
)

checked_required_services=0
for svc in "${required_services[@]}"; do
  if [[ "$TELEMETRY_ONLY" -eq 1 && "$svc" =~ ^(wideRoadEncodeData|livestreamWideRoadEncodeData)$ ]]; then
    continue
  fi
  check_token "$OP_SOURCE_ROOT/cereal/services.py" "$svc"
  check_token "$OP_SOURCE_ROOT/cereal/log.capnp" "$svc"
  checked_required_services=$((checked_required_services + 1))
done

resolve_service_alias() {
  local service
  for service in "$@"; do
    if has_token "$OP_SOURCE_ROOT/cereal/services.py" "$service" && \
       has_token "$OP_SOURCE_ROOT/cereal/log.capnp" "$service"; then
      printf '%s\n' "$service"
      return 0
    fi
  done
  return 1
}

calibration_service="$(resolve_service_alias extrinsicsCalibration liveCalibration || true)"
road_camera_service="$(resolve_service_alias narrowRoadCameraState roadCameraState || true)"
vehicle_parameters_service="$(resolve_service_alias vehicleParameters liveParameters || true)"
road_encode_service="$(resolve_service_alias narrowRoadEncodeData roadEncodeData || true)"
driver_encode_service="$(resolve_service_alias cabinEncodeData driverEncodeData || true)"
livestream_road_encode_service="$(resolve_service_alias livestreamNarrowRoadEncodeData livestreamRoadEncodeData || true)"
livestream_driver_encode_service="$(resolve_service_alias livestreamCabinEncodeData livestreamDriverEncodeData || true)"
[[ -n "$calibration_service" ]] || missing+=("service-alias:calibration(one-of:extrinsicsCalibration liveCalibration)")
[[ -n "$road_camera_service" ]] || missing+=("service-alias:road_camera(one-of:narrowRoadCameraState roadCameraState)")
[[ -n "$vehicle_parameters_service" ]] || missing+=("service-alias:vehicle_parameters(one-of:vehicleParameters liveParameters)")
if [[ "$TELEMETRY_ONLY" -ne 1 ]]; then
  [[ -n "$road_encode_service" ]] || missing+=("service-alias:road_encode(one-of:narrowRoadEncodeData roadEncodeData)")
  [[ -n "$driver_encode_service" ]] || missing+=("service-alias:cabin_encode(one-of:cabinEncodeData driverEncodeData)")
  [[ -n "$livestream_road_encode_service" ]] || missing+=("service-alias:livestream_road_encode(one-of:livestreamNarrowRoadEncodeData livestreamRoadEncodeData)")
  [[ -n "$livestream_driver_encode_service" ]] || missing+=("service-alias:livestream_cabin_encode(one-of:livestreamCabinEncodeData livestreamDriverEncodeData)")
fi

required_capnp_fields=(
  alertText1
  alertText2
  alertType
  laneLineProbs
  laneLineStds
  roadEdgeStds
  leadsV3
  rpyCalib
  height
  calStatus
  calPerc
  wideFromDeviceEuler
  roll
  frameId
  timestampEof
  timestampSof
  sensor
  hasFix
  horizontalAccuracy
  bearingDeg
  unixTimestampMillis
)

for field in "${required_capnp_fields[@]}"; do
  check_token "$OP_SOURCE_ROOT/cereal/log.capnp" "$field"
done

# Live location reads the comma's GPS from openpilot's msgq ring without subscribing
# (src/gps_peek.cpp), so the ring's framing is an interface too: the size tag, the -1 wrap tag,
# 8-byte alignment, and the write pointer moved only after a message is whole.
msgq_root="$OP_SOURCE_ROOT/msgq_repo"
[[ -d "$msgq_root" ]] || msgq_root="$OP_ROOT/msgq_repo"
check_file "$msgq_root/msgq/msgq.h"
check_file "$msgq_root/msgq/msgq.cc"
if [[ -f "$msgq_root/msgq/msgq.cc" ]]; then
  for needle in \
    'uint64_t total_msg_size = ALIGN(msg->size + sizeof(int64_t));' \
    '*(int64_t*)p = -1;' \
    'PACK64(*q->write_pointer, write_cycles, new_ptr);' \
    'q->data = mem + sizeof(msgq_header_t);'; do
    grep -Fq "$needle" "$msgq_root/msgq/msgq.cc" || missing+=("msgq-ring:$needle")
  done
fi

if [[ ${#missing[@]} -gt 0 ]]; then
  printf 'FAIL: upstream interface drift detected:\n' >&2
  printf '  - %s\n' "${missing[@]}" >&2
  exit 1
fi

mkdir -p "$(dirname "$MANIFEST")"
upstream_sha="unknown"
if git -C "$OP_ROOT" rev-parse --short HEAD >/dev/null 2>&1; then
  upstream_sha="$(git -C "$OP_ROOT" rev-parse --short HEAD)"
fi
required_service_count=$((checked_required_services + 3))
if [[ "$TELEMETRY_ONLY" -ne 1 ]]; then
  required_service_count=$((required_service_count + 4))
fi

cat > "$MANIFEST" <<JSON
{
  "opRoot": "${OP_ROOT}",
  "opSourceRoot": "${OP_SOURCE_ROOT}",
  "upstreamSha": "${upstream_sha}",
  "telemetryOnly": $([[ "$TELEMETRY_ONLY" -eq 1 ]] && printf true || printf false),
  "resolvedServices": {
    "calibration": "${calibration_service}",
    "roadCamera": "${road_camera_service}",
    "vehicleParameters": "${vehicle_parameters_service}"
    ,"roadEncode": "${road_encode_service}"
    ,"cabinEncode": "${driver_encode_service}"
    ,"livestreamRoadEncode": "${livestream_road_encode_service}"
    ,"livestreamCabinEncode": "${livestream_driver_encode_service}"
  },
  "checks": {
    "onroadUiExportMethod": "transformer",
    "onroadUiExportFlavor": "${onroad_ui_export_flavor}",
    "requiredTransformerFiles": ${#required_transformer_files[@]},
    "requiredServices": ${required_service_count},
    "requiredCapnpFields": ${#required_capnp_fields[@]},
    "requiredFiles": ${#required_files[@]}
  }
}
JSON

echo "PASS: upstream interface guard"
echo "manifest: $MANIFEST"
