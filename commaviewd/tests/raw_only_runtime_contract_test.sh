#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_CPP="$ROOT/src/bridge_runtime.cc"

[ -f "$BRIDGE_CPP" ] || { echo "FAIL: missing $BRIDGE_CPP"; exit 1; }

grep -Fq "RAW_ONLY_DEFAULT" "$BRIDGE_CPP" || { echo "FAIL: missing RAW_ONLY_DEFAULT startup marker"; exit 1; }
! grep -Fq "g_telemetry_legacy_decode" "$BRIDGE_CPP" || { echo "FAIL: legacy decode rollback switch should be removed"; exit 1; }
! grep -Fq -- "--telemetry-legacy-decode" "$BRIDGE_CPP" || { echo "FAIL: legacy decode CLI flag should be removed"; exit 1; }
! grep -Fq "COMMAVIEW_TELEMETRY_LEGACY_DECODE" "$BRIDGE_CPP" || { echo "FAIL: legacy decode env switch should be removed"; exit 1; }
! grep -Fq -- "--telemetry-blackhole" "$BRIDGE_CPP" || { echo "FAIL: telemetry-blackhole flag should be removed"; exit 1; }
! grep -Fq -- "--telemetry-drain-only" "$BRIDGE_CPP" || { echo "FAIL: telemetry-drain-only flag should be removed"; exit 1; }
! grep -Fq -- "--telemetry-subscribe-only" "$BRIDGE_CPP" || { echo "FAIL: telemetry-subscribe-only flag should be removed"; exit 1; }
! grep -Fq -- "--telem-safe-no-car" "$BRIDGE_CPP" || { echo "FAIL: telem-safe-no-car flag should be removed"; exit 1; }
grep -Fq "send_meta_raw_frame" "$BRIDGE_CPP" || { echo "FAIL: raw telemetry emitter missing"; exit 1; }

echo "PASS: raw-only runtime contract checks passed"
