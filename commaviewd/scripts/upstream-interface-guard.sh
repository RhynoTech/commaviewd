#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/path/to/openpilot-src commaviewd/scripts/upstream-interface-guard.sh [--manifest <path>]
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
)

for f in "${required_files[@]}"; do
  check_file "$f"
done

if [[ ${#missing[@]} -eq 0 ]]; then
  remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
  patch_flavor="openpilot"
  if printf '%s' "$remote" | grep -qi 'sunnypilot'; then
    patch_flavor="sunnypilot"
  fi
  patch_file="$REPO_ROOT/comma4/patches/$patch_flavor/0001-commaview-ui-export-v2.patch"
  check_file "$patch_file"
  if [[ -f "$patch_file" ]] && ! git -C "$OP_ROOT" apply --recount --check "$patch_file" >/dev/null 2>&1 && \
     ! git -C "$OP_ROOT" apply --recount --reverse --check "$patch_file" >/dev/null 2>&1; then
    missing+=("direct-v2-patch:$patch_flavor@$OP_ROOT")
  fi
fi

[[ ${#missing[@]} -eq 0 ]] || {
  printf 'FAIL: upstream interface guard missing current direct-v2 prerequisites:\n' >&2
  printf '  - %s\n' "${missing[@]}" >&2
  exit 1
}

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
    "directV2PatchFlavor": "${patch_flavor}",
    "requiredServices": ${#required_services[@]},
    "requiredCapnpFields": ${#required_capnp_fields[@]},
    "requiredFiles": ${#required_files[@]}
  }
}
JSON

echo "PASS: upstream interface guard"
echo "manifest: $MANIFEST"
