#!/usr/bin/env bash
set -euo pipefail

# msgq never gives a reader slot back (a queue has NUM_READERS), and a subscriber that finds them
# all taken evicts every reader on the queue, openpilot's own included (loggerd on the encoder
# queues). So commaviewd creates no msgq subscriber at all: it reads openpilot's queues through
# src/msgq_ring_reader.cpp, which maps them read-only and keeps its position to itself, and it
# never publishes on them either.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/src"

python3 - "$SRC" <<'PY'
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1])
ring_cpp = src / 'msgq_ring_reader.cpp'
bridge_cc = src / 'bridge_runtime.cc'
for required in (ring_cpp, src / 'msgq_ring_reader.h', bridge_cc):
    if not required.is_file():
        raise SystemExit(f'missing {required}')


def code(path):
    # Prose about msgq in comments must not trip the checks.
    text = re.sub(r'/\*.*?\*/', '', path.read_text(), flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


problems = []
for path in sorted(p for p in src.rglob('*') if p.suffix in {'.cc', '.cpp', '.h', '.hpp'}):
    text = code(path)
    for needle in ('PubSocket', 'PubMaster', 'msgq_init_publisher', 'msgq_msg_send'):
        if needle in text:
            problems.append(f'{path.name}: {needle} (commaviewd never publishes on openpilot queues)')
    # Every way of taking a reader slot, or of opening a queue read-write.
    for needle in ('SubSocket', 'SubMaster', 'msgq_init_subscriber', 'msgq_new_queue', 'msgq_msg_recv',
                   'msgq_poll', 'Context::create', 'Poller::create', 'ServiceReader', '"msgq/msgq.h"',
                   '"msgq/ipc.h"'):
        if needle in text:
            problems.append(f'{path.name}: {needle} (commaviewd creates no msgq subscriber)')

ring = code(ring_cpp)
if 'PROT_READ, MAP_SHARED' not in ring or 'PROT_WRITE' in ring:
    problems.append('msgq_ring_reader.cpp: queues must be mapped PROT_READ only')
if 'O_RDONLY' not in ring or re.search(r'\bO_(RDWR|WRONLY|CREAT|TRUNC)\b', ring):
    problems.append('msgq_ring_reader.cpp: queue files must be opened O_RDONLY only')
if re.search(r'\b(ftruncate|truncate|msync|__atomic_store|__atomic_exchange|__atomic_fetch)', ring):
    problems.append('msgq_ring_reader.cpp: nothing may write to or resize the shared queue')

bridge = code(bridge_cc)
handler = re.search(r'static void handle_video_client\(.*?\n}\n', bridge, re.S)
if handler is None:
    problems.append('bridge_runtime.cc: handle_video_client not found')
else:
    body = handler.group(0)
    if 'commaview::ipc::QueueStreamReader' not in body:
        problems.append('bridge_runtime.cc: handle_video_client must read video through QueueStreamReader')
    if 'msgq_queue_path(video_service)' not in body:
        problems.append('bridge_runtime.cc: the video reader must follow the camera\'s own queue')
    # A gap in the queue (the encoder lapped the reader) resumes on the next keyframe.
    if not re.search(r'if\s*\(\s*discontinuity\s*\)\s*\{[^}]*start_gate\s*=\s*commaview::video::KeyframeStartGate', body, re.S):
        problems.append('bridge_runtime.cc: a reader discontinuity must reset the keyframe gate')

if problems:
    raise SystemExit('\n'.join(problems))
print('PASS: msgq reader slot contract holds (commaviewd creates no msgq subscriber)')
PY
