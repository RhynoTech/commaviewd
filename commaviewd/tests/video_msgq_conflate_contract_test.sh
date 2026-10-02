#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
OP_ROOT="${OP_ROOT:-$REPO_ROOT/../openpilot-src}"

BRIDGE="$ROOT/src/bridge_runtime.cc"
READER="$ROOT/src/msgq_service_reader.cpp"
IMPL_MSGQ="$OP_ROOT/msgq_repo/msgq/impl_msgq.cc"
MSGQ="$OP_ROOT/msgq_repo/msgq/msgq.cc"

if [[ ! -f "$BRIDGE" || ! -f "$READER" ]]; then
  echo "[ERR] Missing bridge runtime or msgq service reader: $BRIDGE $READER" >&2
  exit 2
fi
if [[ ! -f "$IMPL_MSGQ" || ! -f "$MSGQ" ]]; then
  echo "[ERR] Missing openpilot msgq sources under OP_ROOT=$OP_ROOT" >&2
  exit 2
fi

python3 - "$BRIDGE" "$READER" "$IMPL_MSGQ" "$MSGQ" <<'PY'
import pathlib
import re
import sys

bridge = pathlib.Path(sys.argv[1]).read_text()
reader = pathlib.Path(sys.argv[2]).read_text()
impl = pathlib.Path(sys.argv[3]).read_text()
msgq = pathlib.Path(sys.argv[4]).read_text()

# The bridge reads video through its process-lifetime readers, which subscribe conflated.
if not re.search(r'SubSocket::create\s*\([^;]*\bservice_\b[^;]*,\s*true\s*,\s*true\s*(?:,|\))', reader, re.S):
    raise SystemExit('video SubSocket must keep conflate=true in msgq_service_reader.cpp')
if 'commaview::ipc::ServiceReader' not in bridge:
    raise SystemExit('bridge_runtime.cc must read video through commaview::ipc::ServiceReader')
if not re.search(r'if\s*\(\s*conflate\s*\)\s*\{[^}]*q->read_conflate\s*=\s*true\s*;', impl, re.S):
    raise SystemExit('openpilot msgq must map conflate flag to q->read_conflate')
if not re.search(
    r'if\s*\(\s*q->read_conflate\s*\)\s*\{'
    r'.*?new_read_pointer\s*!=\s*write_pointer'
    r'.*?PACK64\s*\(\s*\*q->read_pointers\s*\[\s*id\s*\]\s*,\s*read_cycles\s*,\s*new_read_pointer\s*\)\s*;'
    r'.*?goto\s+start\s*;',
    msgq,
    re.S,
):
    raise SystemExit('openpilot msgq receive path must skip toward latest when read_conflate is true')
print('PASS: video msgq conflate contract holds')
PY
