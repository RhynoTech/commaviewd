#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/path/to/openpilot commaviewd/scripts/upstream-interface-guard.sh [--manifest <path>]
Fast-fails when upstream schema/service interfaces drift in ways that can break CommaViewD.
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
OP_ROOT="${OP_ROOT:-/home/pear/openpilot-src}"
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
MANIFEST="$DIST_DIR/upstream-interface-manifest.json"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest) MANIFEST="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

missing=()

check_file() {
  local p="$1"
  [[ -f "$p" ]] || missing+=("file:$p")
}

check_token() {
  local file="$1"
  local token="$2"
  grep -Eq "(^|[^A-Za-z0-9_])${token}([^A-Za-z0-9_]|$)" "$file" || missing+=("token:$token@$file")
}

required_files=(
  "$OP_ROOT/cereal/log.capnp"
  "$OP_ROOT/cereal/services.py"
  "$OP_ROOT/cereal/messaging/socketmaster.cc"
  "$OP_ROOT/msgq_repo/msgq/msgq.cc"
  "$OP_ROOT/msgq_repo/msgq/event.cc"
  "$OP_ROOT/msgq_repo/msgq/impl_fake.cc"
  "$OP_ROOT/msgq_repo/msgq/impl_msgq.cc"
  "$OP_ROOT/msgq_repo/msgq/ipc.cc"
)

for f in "${required_files[@]}"; do
  check_file "$f"
done

[[ ${#missing[@]} -eq 0 ]] || {
  printf 'FAIL: upstream interface guard missing prerequisites:\n' >&2
  printf '  - %s\n' "${missing[@]}" >&2
  exit 1
}

# Services used by bridge runtime subscriptions.
required_services=(
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
)

for svc in "${required_services[@]}"; do
  check_token "$OP_ROOT/cereal/services.py" "$svc"
  check_token "$OP_ROOT/cereal/log.capnp" "$svc"
done

# Fields/method contracts used by telemetry serialization.
required_capnp_fields=(
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
)

for field in "${required_capnp_fields[@]}"; do
  check_token "$OP_ROOT/cereal/log.capnp" "$field"
done

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

cat > "$MANIFEST" <<JSON
{
  "opRoot": "${OP_ROOT}",
  "upstreamSha": "${upstream_sha}",
  "checks": {
    "requiredServices": ${#required_services[@]},
    "requiredCapnpFields": ${#required_capnp_fields[@]},
    "requiredFiles": ${#required_files[@]}
  }
}
JSON

echo "PASS: upstream interface guard"
echo "manifest: $MANIFEST"
