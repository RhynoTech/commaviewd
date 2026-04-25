#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BRIDGE_CPP="$ROOT/commaviewd/src/bridge_runtime.cc"

assert_contains_fixed() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if ! grep -Fq "$needle" "$file"; then
    echo "FAIL: $message" >&2
    echo "missing: $needle" >&2
    exit 1
  fi
}

assert_contains_fixed "static constexpr uint8_t MSG_VIDEO = 0x05;" "$BRIDGE_CPP" "runtime must define timestamped video frame type"
assert_contains_fixed "ed.getUnixTimestampNanos()" "$BRIDGE_CPP" "runtime must export source video timestamp"
assert_contains_fixed "ed.getWidth()" "$BRIDGE_CPP" "runtime must export source video width"
assert_contains_fixed "ed.getHeight()" "$BRIDGE_CPP" "runtime must export source video height"
assert_contains_fixed "put_be64(&payload[1], timestamp_ns);" "$BRIDGE_CPP" "video v2 frame must include timestamp first"
assert_contains_fixed "put_be32(&payload[9], video_width);" "$BRIDGE_CPP" "video v2 frame must include width"
assert_contains_fixed "put_be32(&payload[13], video_height);" "$BRIDGE_CPP" "video v2 frame must include height"
assert_contains_fixed "put_be32(&payload[17], header_len);" "$BRIDGE_CPP" "video v2 frame must preserve header length"
assert_contains_fixed "payload[0] = MSG_VIDEO;" "$BRIDGE_CPP" "runtime must send timestamped video frames"

echo "timestamped_video_runtime_contract_test: PASS"
