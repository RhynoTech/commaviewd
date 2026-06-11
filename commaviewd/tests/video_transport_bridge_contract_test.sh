#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE="$ROOT/src/bridge_runtime.cc"
ACCOUNTING="$ROOT/src/runtime_video_send_accounting.cpp"
SENDER="$ROOT/src/udp_video_sender.cpp"

if [[ ! -f "$BRIDGE" ]]; then
  echo "[ERR] Missing bridge runtime: $BRIDGE" >&2
  exit 2
fi
if [[ ! -f "$ACCOUNTING" ]]; then
  echo "[ERR] Missing video send accounting module: $ACCOUNTING" >&2
  exit 2
fi
if [[ ! -f "$SENDER" ]]; then
  echo "[ERR] Missing UDP video sender: $SENDER" >&2
  exit 2
fi

python3 - "$BRIDGE" "$ACCOUNTING" "$SENDER" <<'PY'
import pathlib
import re
import sys

bridge = pathlib.Path(sys.argv[1]).read_text()
accounting = pathlib.Path(sys.argv[2]).read_text()
sender = pathlib.Path(sys.argv[3]).read_text()

bridge_required = {
    'UDP sender include': '#include "udp_video_sender.h"',
    'UDP stream id mapper': 'udp_stream_id_for_port',
    'UDP socket creation': 'create_udp_video_socket(port)',
    'UDP sender instance': 'commaview::video::UdpVideoSender udp_video_sender',
    'UDP datagram drain': 'drain_udp_video_control_datagrams',
    'HELLO handling': 'note_client_hello',
    'repair request handling': 'handle_repair_request',
    'UDP frame construction': 'commaview::video::UdpVideoFrameForPacketizing frame',
    'source timestamp reaches UDP frame': 'frame.timestamp_nanos = queued->timestamp_ns;',
    'source width reaches UDP frame': 'frame.width = queued->width;',
    'source height reaches UDP frame': 'frame.height = queued->height;',
    'source keyframe reaches UDP frame': 'frame.is_keyframe = queued->is_keyframe;',
    'UDP send path': 'udp_video_sender.send_frame(frame, runtime_now_ns())',
    'UDP send accounting': 'note_udp_video_send_stats',
    'video queue instance': 'commaview::video::VideoFrameQueue video_queue',
    'HEVC IDR classification': 'commaview::video::contains_hevc_idr',
    'queue push before send': 'video_queue.push',
    'queue pop send path': 'video_queue.pop_next',
    'control frames still consumed': 'consume_client_control_frames(client_fd, &control_state, video_service);',
    'telemetry thread still present': 'telemetry_thread = std::thread(telemetry_loop,',
}
accounting_required = {
    'UDP packet runtime stat': 'udpPacketsSent',
    'UDP send error runtime stat': 'udpSendErrorCount',
    'UDP repair runtime stat': 'udpRepairRequests',
    'UDP repair cache runtime stat': 'udpRepairCacheBytes',
}
sender_required = {
    'UDP sender caches sent packets': 'repair_cache_.store(packets, now_ns);',
    'UDP sender sends datagrams': 'send_fn_(bytes.data(), bytes.size(), endpoint.addr, endpoint.addr_len)',
}
for label, needle in bridge_required.items():
    if needle not in bridge:
        raise SystemExit(f'missing {label}: {needle}')
for label, needle in accounting_required.items():
    if needle not in accounting:
        raise SystemExit(f'missing {label}: {needle}')
for label, needle in sender_required.items():
    if needle not in sender:
        raise SystemExit(f'missing {label}: {needle}')

for forbidden in [
    'const auto chunks = commaview::video::plan_video_chunks(',
    'const auto payload = commaview::video::encode_video_chunk_payload(chunk);',
    'const auto send_result = send_frame_locked(client_fd, payload.data(), payload.size(), &send_mutex);',
    'note_video_chunk_send_result(video_service, chunk, send_result);',
    'note_runtime_peer_disconnect(video_service, "video_chunk_send", send_result);',
]:
    if forbidden in bridge:
        raise SystemExit(f'bridge must not use chunked TCP as primary video path: {forbidden}')

if not re.search(
    r'commaview::video::UdpVideoFrameForPacketizing\s+frame\s*;.*?frame\.stream_id\s*=\s*udp_stream_id\s*;.*?frame\.frame_sequence\s*=\s*.*?queued->sequence.*?frame\.timestamp_nanos\s*=\s*queued->timestamp_ns\s*;.*?frame\.width\s*=\s*queued->width\s*;.*?frame\.height\s*=\s*queued->height\s*;.*?frame\.is_keyframe\s*=\s*queued->is_keyframe\s*;.*?frame\.codec_header\s*=\s*queued->codec_header\s*;.*?frame\.data\s*=\s*queued->data\s*;.*?udp_video_sender\.send_frame\(frame, runtime_now_ns\(\)\)',
    bridge,
    re.S,
):
    raise SystemExit('bridge video path must map queued source metadata into UdpVideoFrameForPacketizing before send_frame')

if not re.search(
    r'int\s+udp_fd\s*=\s*create_udp_video_socket\(port\)\s*;.*?commaview::video::UdpVideoSender\s+udp_video_sender\(',
    bridge,
    re.S,
):
    raise SystemExit('bridge must open the UDP socket before constructing the UDP sender')

print('PASS: UDP video transport bridge contract holds')
PY
