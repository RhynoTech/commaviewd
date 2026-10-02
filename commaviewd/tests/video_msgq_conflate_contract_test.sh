#!/usr/bin/env bash
set -euo pipefail

# The bridge streams the newest encoder frames, never a backlog: each client session starts at the
# queue's write pointer, a reader the encoder laps or that falls seconds behind skips to the newest
# message and resumes on a keyframe. It does so without a msgq subscriber (msgq_ring_reader.h), so
# it relies on the ring's framing rather than on msgq's conflate flag.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE="$ROOT/src/bridge_runtime.cc"
READER="$ROOT/src/msgq_ring_reader.cpp"

if [[ ! -f "$BRIDGE" || ! -f "$READER" ]]; then
  echo "[ERR] Missing bridge runtime or msgq ring reader: $BRIDGE $READER" >&2
  exit 2
fi

python3 - "$BRIDGE" "$READER" <<'PY'
import pathlib
import re
import sys

bridge = pathlib.Path(sys.argv[1]).read_text()
reader = pathlib.Path(sys.argv[2]).read_text()

if 'commaview::ipc::QueueStreamReader video_reader(' not in bridge:
    raise SystemExit('bridge_runtime.cc must read video through commaview::ipc::QueueStreamReader')
# A session starts at the write pointer: nothing published before it is sent.
if not re.search(r'if\s*\(\s*!cursor_valid_\s*\)\s*\{\s*cursor_\s*=\s*write_pointer\s*;', reader):
    raise SystemExit('msgq_ring_reader.cpp: a reader must start (and start over) at the write pointer')
# Seconds behind is skipped, not streamed late.
if not re.search(r'if\s*\(\s*behind\s*>\s*data_size\s*/\s*4\s*\)\s*\{\s*resync\(Resync::kBehind\)', reader):
    raise SystemExit('msgq_ring_reader.cpp: a reader far behind the writer must skip to the newest message')
print('PASS: video msgq newest-frame contract holds')
PY
