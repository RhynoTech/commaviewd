#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE="$ROOT/src/bridge_runtime.cc"
ACCOUNTING="$ROOT/src/runtime_video_send_accounting.cpp"

if [[ ! -f "$BRIDGE" ]]; then
  echo "[ERR] Missing bridge runtime: $BRIDGE" >&2
  exit 2
fi
if [[ ! -f "$ACCOUNTING" ]]; then
  echo "[ERR] Missing video send accounting module: $ACCOUNTING" >&2
  exit 2
fi

python3 - "$BRIDGE" "$ACCOUNTING" <<'PY'
import pathlib
import re
import sys

bridge = pathlib.Path(sys.argv[1]).read_text()
accounting = pathlib.Path(sys.argv[2]).read_text()

bridge_required = {
    'video transport policy include': '#include "video_transport_policy.h"',
    'chunk planner in send path': 'const auto chunks = commaview::video::plan_video_chunks(',
    'chunk payload encoding in send path': 'const auto payload = commaview::video::encode_video_chunk_payload(chunk);',
    'encoded chunk send path': 'const auto send_result = send_frame_locked(client_fd, payload.data(), payload.size(), &send_mutex);',
    'chunked frame abandon accounting': 'frame_abandon_count',
    'video queue instance': 'commaview::video::VideoFrameQueue video_queue',
    'HEVC IDR classification': 'commaview::video::contains_hevc_idr',
    'queue push before send': 'video_queue.push',
    'queue pop send path': 'video_queue.pop_next',
    'zero-byte backpressure keyframe wait': 'video_queue.note_backpressure_without_partial_send',
}
accounting_required = {
    'queue drop runtime stat': 'queueDropCount',
    'queue high watermark runtime stat': 'queueHighWatermark',
    'keyframe wait drop runtime stat': 'keyframeWaitDropCount',
    'zero-byte recovered runtime stat': 'zeroByteBackpressureRecoveredCount',
    'queued frame age runtime stat': 'maxQueuedFrameAgeMs',
}
for label, needle in bridge_required.items():
    if needle not in bridge:
        raise SystemExit(f'missing {label}: {needle}')
if 'Legacy contract marker: video used to call send_frame_locked' in bridge:
    raise SystemExit('old whole-frame contract marker should be gone')
if 'send_buffers_locked(client_fd' in bridge:
    raise SystemExit('old whole-frame scatter/gather send path should be gone')
for label, needle in accounting_required.items():
    if needle not in accounting:
        raise SystemExit(f'missing {label}: {needle}')

if not re.search(
    r'const\s+auto\s+chunks\s*=\s*commaview::video::plan_video_chunks\s*\(.*?\)\s*;\s*.*?for\s*\([^)]*const\s+auto&\s+chunk\s*:\s*chunks\s*\)\s*\{\s*const\s+auto\s+payload\s*=\s*commaview::video::encode_video_chunk_payload\s*\(\s*chunk\s*\)\s*;\s*const\s+auto\s+send_result\s*=\s*send_frame_locked\s*\(\s*client_fd\s*,\s*payload\.data\s*\(\s*\)\s*,\s*payload\.size\s*\(\s*\)\s*,\s*&send_mutex\s*\)',
    bridge,
    re.S,
):
    raise SystemExit('bridge video send path must plan chunks, encode each chunk payload, then send encoded payload bytes')

if not re.search(
    r'if\s*\(\s*send_result\.status\s*==\s*commaview::net::SendStatus::Backpressure\s*&&\s*send_result\.bytes_sent\s*==\s*0\s*\)\s*\{[^}]*video_queue\.note_backpressure_without_partial_send\s*\(',
    bridge,
    re.S,
):
    raise SystemExit('zero-byte video backpressure must enter keyframe wait before continuing')

print('PASS: video transport bridge contract holds')
PY
