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
    'chunked video payload marker': 'MSG_VIDEO_CHUNK',
    'chunk planner before send': 'plan_video_chunks',
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
for label, needle in accounting_required.items():
    if needle not in accounting:
        raise SystemExit(f'missing {label}: {needle}')

if not re.search(
    r'if\s*\(\s*send_result\.status\s*==\s*commaview::net::SendStatus::Backpressure\s*&&\s*send_result\.bytes_sent\s*==\s*0\s*\)\s*\{[^}]*video_queue\.note_backpressure_without_partial_send\s*\(',
    bridge,
    re.S,
):
    raise SystemExit('zero-byte video backpressure must enter keyframe wait before continuing')

print('PASS: video transport bridge contract holds')
PY
