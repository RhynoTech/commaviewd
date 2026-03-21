#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BRIDGE_CPP="$ROOT/src/bridge_runtime.cc"

source "$SCRIPT_DIR/runtime_debug_policy_contract_test.sh"

[ -f "$BRIDGE_CPP" ] || fail "missing $BRIDGE_CPP"

assert_contains_fixed "RAW_ONLY_DEFAULT" "$BRIDGE_CPP" "missing RAW_ONLY_DEFAULT startup marker"
assert_not_contains_fixed "g_telemetry_legacy_decode" "$BRIDGE_CPP" "legacy decode rollback switch should be removed"
assert_not_contains_fixed "--telemetry-legacy-decode" "$BRIDGE_CPP" "legacy decode CLI flag should be removed"
assert_not_contains_fixed "COMMAVIEW_TELEMETRY_LEGACY_DECODE" "$BRIDGE_CPP" "legacy decode env switch should be removed"
assert_not_contains_fixed "--telemetry-blackhole" "$BRIDGE_CPP" "telemetry-blackhole flag should be removed"
assert_not_contains_fixed "--telemetry-drain-only" "$BRIDGE_CPP" "telemetry-drain-only flag should be removed"
assert_not_contains_fixed "--telemetry-subscribe-only" "$BRIDGE_CPP" "telemetry-subscribe-only flag should be removed"
assert_not_contains_fixed "--telem-safe-no-car" "$BRIDGE_CPP" "telem-safe-no-car flag should be removed"
assert_contains_fixed "send_meta_raw_frame" "$BRIDGE_CPP" "raw telemetry emitter missing"
assert_not_contains_fixed "extract_log_mono_time_from_raw_event" "$BRIDGE_CPP" "raw-only bridge must not extract logMonoTime from raw events"
assert_not_contains_fixed "event_which_for_service_index" "$BRIDGE_CPP" "raw-only bridge must not derive event type metadata on comma side"
assert_not_contains_fixed "std::unique_ptr<Message> newer(sock->receive(true));" "$BRIDGE_CPP" "manual telemetry drain loop should be removed for conflated sockets"
assert_carstate_sampled_runtime_contract "$BRIDGE_CPP"
assert_not_contains_fixed "raw_log_mono[raw_idx] = event.getLogMonoTime();" "$BRIDGE_CPP" "raw-only telemetry path should not read logMonoTime via capnp"
assert_not_contains_fixed "raw_event_which[raw_idx] = static_cast<uint16_t>(which);" "$BRIDGE_CPP" "raw-only telemetry path should not decode event type via capnp"
assert_not_contains_fixed "--dev" "$BRIDGE_CPP" "--dev debug flag should be removed"
assert_not_contains_fixed "--telem-emit-ms" "$BRIDGE_CPP" "telem-emit-ms override flag should be removed"
assert_not_contains_fixed "COMMAVIEW_TELEMETRY_EMIT_MS" "$BRIDGE_CPP" "COMMAVIEW_TELEMETRY_EMIT_MS override env should be removed"
assert_not_contains_fixed "build_telemetry_json" "$BRIDGE_CPP" "legacy telemetry JSON builder should be removed from bridge runtime"
assert_not_contains_fixed "encode_car_state_typed" "$BRIDGE_CPP" "legacy typed telemetry encoder helpers should be removed"
assert_not_contains_fixed "send_meta_json" "$BRIDGE_CPP" "legacy json emitter helper should be removed"
assert_not_contains_fixed "raw_latest(NUM_TELEM)" "$BRIDGE_CPP" "telemetry cache/resend path should be removed in favor of throttled reads"
assert_not_contains_fixed "have_raw(NUM_TELEM" "$BRIDGE_CPP" "telemetry cache presence flags should be removed"
assert_contains_fixed "next_telem_poll" "$BRIDGE_CPP" "telemetry poll throttle deadline missing"
assert_contains_fixed "telemetry_poller" "$BRIDGE_CPP" "telemetry should use a dedicated throttled poller"

echo "PASS: raw-only runtime contract checks passed"
