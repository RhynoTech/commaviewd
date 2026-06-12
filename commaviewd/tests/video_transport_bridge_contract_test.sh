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
    'UDP socket creation': 'create_udp_video_socket(PORT_ROAD)',
    'UDP sender instance': 'commaview::video::UdpVideoSender udp_video_sender',
    'UDP datagram drain': 'drain_udp_video_control_datagrams',
    'HELLO handling': 'note_client_hello',
    'policy/suppress handling': 'note_client_policy',
    'client liveness gate': 'has_active_client',
    'suppress flag honored before packetizing': 'client_suppresses_video',
    'bounded UDP datagram send': 'send_udp_video_datagram',
    'standalone UDP video pump': 'static void udp_video_stream_loop(int udp_fd, int port, const char* video_service)',
    'road UDP pump started at startup': 'threads.emplace_back(udp_video_stream_loop, road_udp_fd, PORT_ROAD, video_services[0]);',
    'wide UDP pump started at startup': 'threads.emplace_back(udp_video_stream_loop, wide_udp_fd, PORT_WIDE, video_services[1]);',
    'driver UDP pump started at startup': 'threads.emplace_back(udp_video_stream_loop, driver_udp_fd, PORT_DRIVER, video_services[2]);',
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
    '#include "video_' + 'chunk_protocol.h"',
    'commaview::video::Video' + 'Chunk',
    'plan_video_' + 'chunks(',
    'encode_video_' + 'chunk_payload(',
    'send_frame_locked(client_fd,',
    'note_video_' + 'chunk_send_result(',
]:
    if forbidden in bridge:
        raise SystemExit(f'bridge must not use retired TCP camera video path: {forbidden}')

if not re.search(
    r'commaview::video::UdpVideoFrameForPacketizing\s+frame\s*;.*?frame\.stream_id\s*=\s*udp_stream_id\s*;.*?frame\.frame_sequence\s*=\s*.*?queued->sequence.*?frame\.timestamp_nanos\s*=\s*queued->timestamp_ns\s*;.*?frame\.width\s*=\s*queued->width\s*;.*?frame\.height\s*=\s*queued->height\s*;.*?frame\.is_keyframe\s*=\s*queued->is_keyframe\s*;.*?frame\.codec_header\s*=\s*queued->codec_header\s*;.*?frame\.data\s*=\s*queued->data\s*;.*?udp_video_sender\.send_frame\(frame, runtime_now_ns\(\)\)',
    bridge,
    re.S,
):
    raise SystemExit('bridge video path must map queued source metadata into UdpVideoFrameForPacketizing before send_frame')

if not re.search(
    r'road_udp_fd\s*=\s*create_udp_video_socket\(PORT_ROAD\).*?wide_udp_fd\s*=\s*create_udp_video_socket\(PORT_WIDE\).*?driver_udp_fd\s*=\s*create_udp_video_socket\(PORT_DRIVER\).*?threads\.emplace_back\(udp_video_stream_loop, road_udp_fd, PORT_ROAD, video_services\[0\]\).*?append_runtime_run_event\("ready"\)',
    bridge,
    re.S,
):
    raise SystemExit('bridge must bind UDP sockets synchronously before video threads and ready')

if not re.search(
    r'static void udp_video_stream_loop\(int udp_fd, int port, const char\* video_service\).*?commaview::video::UdpVideoSender\s+udp_video_sender\(',
    bridge,
    re.S,
):
    raise SystemExit('bridge must construct the UDP sender from a pre-bound socket')


def extract_function_body(src, signature):
    start = src.find(signature)
    if start < 0:
        raise SystemExit(f'expected function: {signature}')
    brace = src.find('{', start)
    depth = 0
    for pos in range(brace, len(src)):
        ch = src[pos]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return src[brace + 1:pos]
    raise SystemExit(f'unterminated function: {signature}')


udp_pump = extract_function_body(bridge, 'static void udp_video_stream_loop(int udp_fd, int port, const char* video_service)')
if 'client_fd' in udp_pump or 'client_socket_alive' in udp_pump:
    raise SystemExit('UDP video pump must not depend on a TCP client connection')

print('PASS: UDP video transport bridge contract holds')
PY
