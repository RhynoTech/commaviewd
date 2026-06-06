#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BRIDGE="$ROOT/commaviewd/src/bridge_runtime.cc"

grep -Fq 'runtime-run-events.jsonl' "$BRIDGE" || { echo "missing runtime-run-events path" >&2; exit 1; }
grep -Fq 'append_runtime_run_event' "$BRIDGE" || { echo "missing lifecycle append helper" >&2; exit 1; }
grep -Fq 'process_start' "$BRIDGE" || { echo "missing process_start event" >&2; exit 1; }
grep -Fq 'client_connected' "$BRIDGE" || { echo "missing client_connected event" >&2; exit 1; }
grep -Fq 'client_disconnected' "$BRIDGE" || { echo "missing client_disconnected event" >&2; exit 1; }
grep -Fq 'peer_disconnect' "$BRIDGE" || { echo "missing peer_disconnect event" >&2; exit 1; }

echo "PASS: runtime lifecycle log contract"
