#!/usr/bin/env bash
set -euo pipefail

# msgq never gives a reader slot back (a queue has 15), and a subscriber that finds them all taken
# evicts every reader on the queue, openpilot's own included. So commaviewd subscribes only through
# commaview::ipc::ServiceReader, once per process, and never publishes on openpilot's queues.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT/src"

python3 - "$SRC" <<'PY'
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1])
reader_cpp = src / 'msgq_service_reader.cpp'
bridge_cc = src / 'bridge_runtime.cc'
for required in (reader_cpp, bridge_cc):
    if not required.is_file():
        raise SystemExit(f'missing {required}')


def code(path):
    # Prose about msgq in comments must not trip the checks.
    return re.sub(r'//[^\n]*', '', path.read_text())


problems = []
for path in sorted(p for p in src.rglob('*') if p.suffix in {'.cc', '.cpp', '.h', '.hpp'}):
    text = code(path)
    for needle in ('PubSocket', 'PubMaster', 'msgq_init_publisher', 'msgq_msg_send'):
        if needle in text:
            problems.append(f'{path.name}: {needle} (commaviewd never publishes on openpilot queues)')
    if path != reader_cpp:
        for needle in ('SubSocket::create', 'SubMaster', 'msgq_init_subscriber', 'Context::create', 'Poller::create'):
            if needle in text:
                problems.append(f'{path.name}: {needle} (subscribe only through commaview::ipc::ServiceReader)')

reader = code(reader_cpp)
if reader.count('SubSocket::create') != 1:
    problems.append('msgq_service_reader.cpp: expected exactly one SubSocket::create')
# Made on the reader's own long-lived thread: msgq signals the thread that subscribed.
run_parts = reader.split('void ServiceReader::run()', 1)
if len(run_parts) != 2 or 'SubSocket::create' not in run_parts[1]:
    problems.append('msgq_service_reader.cpp: the subscriber must be created in ServiceReader::run')

bridge = code(bridge_cc)
handler = re.search(r'static void handle_video_client\(.*?\n}\n', bridge, re.S)
if handler is None:
    problems.append('bridge_runtime.cc: handle_video_client not found')
else:
    body = handler.group(0)
    if 'new commaview::ipc::ServiceReader' in body:
        problems.append('bridge_runtime.cc: a client connection must not create a reader')
    for needle in ('begin_session()', 'end_session()', 'next_message('):
        if needle not in body:
            problems.append(f'bridge_runtime.cc: handle_video_client must use {needle}')
if re.search(r'\bdelete\s+(?:g_video_readers|video_reader)\b', bridge):
    problems.append('bridge_runtime.cc: video readers must live for the whole process')

if problems:
    raise SystemExit('\n'.join(problems))
print('PASS: msgq reader slot contract holds')
PY
