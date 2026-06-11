#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BRIDGE_CPP="$ROOT/commaviewd/src/bridge_runtime.cc"
UDP_PACKETIZER_H="$ROOT/commaviewd/src/udp_video_packetizer.h"
UDP_PACKETIZER_CPP="$ROOT/commaviewd/src/udp_video_packetizer.cpp"
UDP_PROTOCOL_CPP="$ROOT/commaviewd/src/udp_video_protocol.cpp"

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

assert_contains_fixed "uint64_t timestamp_nanos = 0;" "$UDP_PACKETIZER_H" "UDP frame must carry source video timestamp"
assert_contains_fixed "uint32_t width = 0;" "$UDP_PACKETIZER_H" "UDP frame must carry source video width"
assert_contains_fixed "uint32_t height = 0;" "$UDP_PACKETIZER_H" "UDP frame must carry source video height"
assert_contains_fixed "bool is_keyframe = false;" "$UDP_PACKETIZER_H" "UDP frame must carry source keyframe metadata"
assert_contains_fixed "packet.timestamp_nanos = frame.timestamp_nanos;" "$UDP_PACKETIZER_CPP" "UDP packetizer must preserve timestamp"
assert_contains_fixed "packet.width = frame.width;" "$UDP_PACKETIZER_CPP" "UDP packetizer must preserve width"
assert_contains_fixed "packet.height = frame.height;" "$UDP_PACKETIZER_CPP" "UDP packetizer must preserve height"
assert_contains_fixed "append_u64_be(out, packet.timestamp_nanos);" "$UDP_PROTOCOL_CPP" "UDP payload must include timestamp"
assert_contains_fixed "append_u32_be(out, packet.width);" "$UDP_PROTOCOL_CPP" "UDP payload must include width"
assert_contains_fixed "append_u32_be(out, packet.height);" "$UDP_PROTOCOL_CPP" "UDP payload must include height"
assert_contains_fixed "ed.getUnixTimestampNanos()" "$BRIDGE_CPP" "runtime must read source video timestamp"
assert_contains_fixed "ed.getWidth()" "$BRIDGE_CPP" "runtime must read source video width"
assert_contains_fixed "ed.getHeight()" "$BRIDGE_CPP" "runtime must read source video height"
assert_contains_fixed "frame.timestamp_nanos = queued->timestamp_ns;" "$BRIDGE_CPP" "bridge must pass timestamp into UDP packetizer"
assert_contains_fixed "frame.width = queued->width;" "$BRIDGE_CPP" "bridge must pass width into UDP packetizer"
assert_contains_fixed "frame.height = queued->height;" "$BRIDGE_CPP" "bridge must pass height into UDP packetizer"
assert_contains_fixed "frame.is_keyframe = queued->is_keyframe;" "$BRIDGE_CPP" "bridge must pass keyframe metadata into UDP packetizer"
assert_contains_fixed "frame.codec_header = queued->codec_header;" "$BRIDGE_CPP" "bridge must pass codec header into UDP packetizer"
assert_contains_fixed "frame.data = queued->data;" "$BRIDGE_CPP" "bridge must pass frame bytes into UDP packetizer"
assert_contains_fixed "udp_video_sender.send_frame(frame, runtime_now_ns())" "$BRIDGE_CPP" "runtime bridge must send timestamped UDP video frames"
assert_not_contains_fixed "send_frame_locked(" "$BRIDGE_CPP" "runtime bridge should not keep the old TCP video send helper"

echo "timestamped_video_runtime_contract_test: PASS"
