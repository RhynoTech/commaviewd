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
grep -Fq "extract_log_mono_time_from_raw_event" "$BRIDGE_CPP" || { echo "FAIL: raw logMono extractor missing"; exit 1; }
! grep -Fq "std::unique_ptr<Message> newer(sock->receive(true));" "$BRIDGE_CPP" || { echo "FAIL: manual telemetry drain loop should be removed for conflated sockets"; exit 1; }
! grep -Fq "raw_log_mono[raw_idx] = event.getLogMonoTime();" "$BRIDGE_CPP" || { echo "FAIL: raw-only telemetry path should not read logMonoTime via capnp"; exit 1; }
! grep -Fq "raw_event_which[raw_idx] = static_cast<uint16_t>(which);" "$BRIDGE_CPP" || { echo "FAIL: raw-only telemetry path should derive event type from service index"; exit 1; }
! grep -Fq -- "--dev" "$BRIDGE_CPP" || { echo "FAIL: --dev debug flag should be removed"; exit 1; }
! grep -Fq -- "--telem-emit-ms" "$BRIDGE_CPP" || { echo "FAIL: telem-emit-ms override flag should be removed"; exit 1; }
! grep -Fq "COMMAVIEW_TELEMETRY_EMIT_MS" "$BRIDGE_CPP" || { echo "FAIL: COMMAVIEW_TELEMETRY_EMIT_MS override env should be removed"; exit 1; }
! grep -Fq "build_telemetry_json" "$BRIDGE_CPP" || { echo "FAIL: legacy telemetry JSON builder should be removed from bridge runtime"; exit 1; }
! grep -Fq "encode_car_state_typed" "$BRIDGE_CPP" || { echo "FAIL: legacy typed telemetry encoder helpers should be removed"; exit 1; }
! grep -Fq "send_meta_json" "$BRIDGE_CPP" || { echo "FAIL: legacy json emitter helper should be removed"; exit 1; }

echo "PASS: raw-only runtime contract checks passed"
