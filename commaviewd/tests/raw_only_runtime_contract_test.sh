#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_CPP="$ROOT/src/bridge_runtime.cc"

[ -f "$BRIDGE_CPP" ] || { echo "FAIL: missing $BRIDGE_CPP"; exit 1; }

grep -Fq "RAW_ONLY_DEFAULT" "$BRIDGE_CPP" || { echo "FAIL: missing RAW_ONLY_DEFAULT startup marker"; exit 1; }
grep -Fq "g_telemetry_legacy_decode" "$BRIDGE_CPP" || { echo "FAIL: missing legacy decode rollback switch"; exit 1; }
grep -Fq "if (g_telemetry_legacy_decode)" "$BRIDGE_CPP" || { echo "FAIL: legacy decode guard not found"; exit 1; }
grep -Fq -- "--telemetry-legacy-decode" "$BRIDGE_CPP" || { echo "FAIL: missing legacy decode CLI flag"; exit 1; }
grep -Fq "COMMAVIEW_TELEMETRY_LEGACY_DECODE" "$BRIDGE_CPP" || { echo "FAIL: missing legacy decode env switch"; exit 1; }
grep -Fq "send_meta_raw_frame" "$BRIDGE_CPP" || { echo "FAIL: raw telemetry emitter missing"; exit 1; }

echo "PASS: raw-only runtime contract checks passed"
