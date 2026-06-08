#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BRIDGE_CPP="$ROOT/commaviewd/src/bridge_runtime.cc"
CHUNK_HEADER="$ROOT/commaviewd/src/video_chunk_protocol.h"
CHUNK_CPP="$ROOT/commaviewd/src/video_chunk_protocol.cpp"

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

assert_not_contains_fixed() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if grep -Fq "$needle" "$file"; then
    echo "FAIL: $message" >&2
    echo "unexpected: $needle" >&2
    exit 1
  fi
}

assert_contains_fixed "static constexpr uint8_t MSG_VIDEO_CHUNK = 0x06;" "$CHUNK_HEADER" "runtime must define chunked video frame type"
assert_contains_fixed "const auto chunks = commaview::video::plan_video_chunks(" "$BRIDGE_CPP" "runtime bridge must plan chunks in the video send path"
assert_contains_fixed "const auto payload = commaview::video::encode_video_chunk_payload(chunk);" "$BRIDGE_CPP" "runtime bridge must encode chunk payloads before sending"
assert_contains_fixed "const auto send_result = send_frame_locked(client_fd, payload.data(), payload.size(), &send_mutex);" "$BRIDGE_CPP" "runtime bridge must send encoded chunk payloads"
assert_contains_fixed "frame_abandon_count" "$BRIDGE_CPP" "runtime must track abandoned chunked frames"
assert_not_contains_fixed "Legacy contract marker: video used to call send_frame_locked" "$BRIDGE_CPP" "old whole-frame contract marker should be gone"
assert_not_contains_fixed "send_buffers_locked(client_fd" "$BRIDGE_CPP" "old whole-frame scatter/gather send path should be gone"
assert_contains_fixed "ed.getUnixTimestampNanos()" "$BRIDGE_CPP" "runtime must export source video timestamp"
assert_contains_fixed "ed.getWidth()" "$BRIDGE_CPP" "runtime must export source video width"
assert_contains_fixed "ed.getHeight()" "$BRIDGE_CPP" "runtime must export source video height"
assert_contains_fixed "frame.timestamp_ns = queued->timestamp_ns;" "$BRIDGE_CPP" "bridge must pass timestamp into chunk planner"
assert_contains_fixed "frame.width = queued->width;" "$BRIDGE_CPP" "bridge must pass width into chunk planner"
assert_contains_fixed "frame.height = queued->height;" "$BRIDGE_CPP" "bridge must pass height into chunk planner"
assert_contains_fixed "chunk.timestamp_ns = frame.timestamp_ns;" "$CHUNK_CPP" "chunk planner must preserve timestamp"
assert_contains_fixed "append_u64_be(payload, chunk.timestamp_ns);" "$CHUNK_CPP" "chunk payload must include timestamp"
assert_contains_fixed "append_u32_be(payload, chunk.width);" "$CHUNK_CPP" "chunk payload must include width"
assert_contains_fixed "append_u32_be(payload, chunk.height);" "$CHUNK_CPP" "chunk payload must include height"
assert_contains_fixed "append_u32_be(payload, chunk.codec_header_len);" "$CHUNK_CPP" "chunk payload must preserve header length"
assert_contains_fixed "payload.push_back(MSG_VIDEO_CHUNK);" "$CHUNK_CPP" "runtime must send chunked timestamped video frames"

echo "timestamped_video_runtime_contract_test: PASS"
