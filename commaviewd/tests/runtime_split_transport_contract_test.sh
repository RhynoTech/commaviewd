#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE="$ROOT/src/bridge_runtime.cc"
POLICY="$ROOT/src/policy.cpp"

grep -q 'PORT_TELEMETRY = 8203' "$BRIDGE"
grep -q 'handle_telemetry_client' "$BRIDGE"
grep -q 'telemetry_on_video' "$BRIDGE"
grep -q 'transportVersion' "$POLICY"
grep -q 'clientRole' "$POLICY"

python3 - "$BRIDGE" <<'PY'
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1]).read_text()

def extract_function(name: str) -> str:
    needle = f"static void {name}()"
    start = src.find(needle)
    if start < 0:
        raise SystemExit(f"expected {name}() helper")
    brace = src.find("{", start)
    if brace < 0:
        raise SystemExit(f"expected {name}() body")
    depth = 0
    for pos in range(brace, len(src)):
        ch = src[pos]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return src[brace + 1:pos]
    raise SystemExit(f"unterminated {name}() body")

body = extract_function("flush_runtime_state")
lock_pos = body.find("std::lock_guard<std::mutex> lock(g_runtime_state_mutex);")
if lock_pos < 0:
    raise SystemExit("expected flush_runtime_state() to snapshot under g_runtime_state_mutex")
write_positions = [match.start() for match in re.finditer(r"\bwrite_text_file_best_effort\b", body)]
if not write_positions:
    raise SystemExit("expected flush_runtime_state() to write rendered runtime state")

lock_block_start = body.rfind("{", 0, lock_pos)
if lock_block_start < 0:
    raise SystemExit("expected scoped lock block in flush_runtime_state()")
depth = 0
lock_block_end = -1
for pos in range(lock_block_start, len(body)):
    ch = body[pos]
    if ch == "{":
        depth += 1
    elif ch == "}":
        depth -= 1
        if depth == 0:
            lock_block_end = pos
            break
if lock_block_end < 0:
    raise SystemExit("expected scoped lock block to close before runtime state writes")
if any(pos < lock_block_end for pos in write_positions):
    raise SystemExit("runtime stats writes must happen after releasing g_runtime_state_mutex")

print("PASS: runtime stats flush writes outside lock")
PY

printf 'PASS: runtime split transport contract holds\n'
