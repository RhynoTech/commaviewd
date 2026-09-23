#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_script="$root/scripts/build-ubuntu.sh"

grep -Fq 'grep -Eq '\''narrowRoadEncodeData|cabinEncodeData'\'' "$OP_SOURCE_ROOT/cereal/services.py"' "$build_script"
if sed -n '/VIDEO_SCHEMA_FLAGS=()/,/echo "\[1\/5\]/p' "$build_script" | grep -Fq 'log.capnp'; then
  echo "FAIL: encoded-video service-name detection still depends on log.capnp" >&2
  exit 1
fi

echo "PASS: build detects encoded-video service names from services.py"
