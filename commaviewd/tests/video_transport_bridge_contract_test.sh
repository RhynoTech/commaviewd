#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE="$ROOT/src/bridge_runtime.cc"

if [[ ! -f "$BRIDGE" ]]; then
  echo "[ERR] Missing bridge runtime: $BRIDGE" >&2
  exit 2
fi

python3 - "$BRIDGE" <<'PY'
import pathlib
import re
import sys

bridge = pathlib.Path(sys.argv[1]).read_text()

required = {
    'video transport policy include': '#include "video_transport_policy.h"',
    'video queue instance': 'commaview::video::VideoFrameQueue video_queue',
    'HEVC IDR classification': 'commaview::video::contains_hevc_idr',
    'queue push before send': 'video_queue.push',
    'queue pop send path': 'video_queue.pop_next',
    'zero-byte backpressure keyframe wait': 'video_queue.note_backpressure_without_partial_send',
    'queue drop runtime stat': 'queueDropCount',
    'queue high watermark runtime stat': 'queueHighWatermark',
    'keyframe wait drop runtime stat': 'keyframeWaitDropCount',
    'zero-byte recovered runtime stat': 'zeroByteBackpressureRecoveredCount',
    'queued frame age runtime stat': 'maxQueuedFrameAgeMs',
}
for label, needle in required.items():
    if needle not in bridge:
        raise SystemExit(f'missing {label}: {needle}')

if not re.search(
    r'if\s*\(\s*send_result\.status\s*==\s*commaview::net::SendStatus::Backpressure\s*&&\s*send_result\.bytes_sent\s*==\s*0\s*\)\s*\{[^}]*video_queue\.note_backpressure_without_partial_send\s*\(',
    bridge,
    re.S,
):
    raise SystemExit('zero-byte video backpressure must enter keyframe wait before continuing')

print('PASS: video transport bridge contract holds')
PY
