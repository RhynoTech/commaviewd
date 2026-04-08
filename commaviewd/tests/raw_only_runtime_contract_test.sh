#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BRIDGE_CPP="$ROOT/src/bridge_runtime.cc"

source "$SCRIPT_DIR/runtime_debug_policy_contract_test.sh"

[ -f "$BRIDGE_CPP" ] || fail "missing $BRIDGE_CPP"

assert_contains_fixed 'RAW_ONLY_DEFAULT' "$BRIDGE_CPP" 'missing RAW_ONLY_DEFAULT startup marker'
assert_contains_fixed 'DIRECT_V2_UI_EXPORT_DEFAULT' "$BRIDGE_CPP" 'missing DIRECT_V2_UI_EXPORT_DEFAULT startup marker'
assert_contains_fixed '"commaViewControl"' "$BRIDGE_CPP" 'missing control subscription'
assert_contains_fixed '"commaViewScene"' "$BRIDGE_CPP" 'missing scene subscription'
assert_contains_fixed '"commaViewStatus"' "$BRIDGE_CPP" 'missing status subscription'
assert_not_contains_fixed '"commaViewHudLite"' "$BRIDGE_CPP" 'HUD-lite service should be gone'
assert_not_contains_fixed '"carState"' "$BRIDGE_CPP" 'legacy carState bridge subscription should be removed'
assert_not_contains_fixed '"selfdriveState"' "$BRIDGE_CPP" 'legacy selfdriveState bridge subscription should be removed'
assert_not_contains_fixed '"deviceState"' "$BRIDGE_CPP" 'legacy deviceState bridge subscription should be removed'
assert_not_contains_fixed '"liveCalibration"' "$BRIDGE_CPP" 'legacy liveCalibration bridge subscription should be removed'
assert_not_contains_fixed '"radarState"' "$BRIDGE_CPP" 'legacy radarState bridge subscription should be removed'
assert_not_contains_fixed '"modelV2"' "$BRIDGE_CPP" 'legacy modelV2 bridge subscription should be removed'
assert_not_contains_fixed 'telemetry_index_for_which' "$BRIDGE_CPP" 'legacy event->service index mapping should be removed'
assert_not_contains_fixed 'car_state_idx' "$BRIDGE_CPP" 'legacy carState sample special-case should be removed'
assert_not_contains_fixed 'sampled_latest(NUM_TELEM)' "$BRIDGE_CPP" 'legacy sampled cache should be removed for HUD-lite-only path'
assert_not_contains_fixed 'sampled_have_latest(NUM_TELEM' "$BRIDGE_CPP" 'legacy sampled cache flags should be removed for HUD-lite-only path'
assert_contains_fixed 'send_meta_raw_frame' "$BRIDGE_CPP" 'raw telemetry emitter missing'
assert_contains_fixed 'std::vector<uint8_t> payload(1 + 1 + 4 + raw_len);' "$BRIDGE_CPP" 'raw envelope should include version byte plus service index and length'
assert_contains_fixed 'payload[0] = envelope_version;' "$BRIDGE_CPP" 'raw envelope should write the selected envelope version'
assert_contains_fixed 'RAW_META_ENVELOPE_V4' "$BRIDGE_CPP" 'v4 capnp raw envelope constant missing'
assert_contains_fixed 'RAW_META_ENVELOPE_V5' "$BRIDGE_CPP" 'v5 json raw envelope constant missing'
assert_contains_fixed 'payload[1] = service_index;' "$BRIDGE_CPP" 'raw envelope should store service index after version byte'
assert_contains_fixed 'put_be32(&payload[2], raw_len);' "$BRIDGE_CPP" 'raw envelope should write length after version and service index'
assert_contains_fixed 'std::thread telemetry_thread' "$BRIDGE_CPP" 'telemetry should run in a dedicated thread'
assert_contains_fixed "std::mutex send_mutex;" "$BRIDGE_CPP" "per-client send mutex missing"
assert_contains_fixed "send_frame_locked(client_fd, payload.data(), payload.size(), &send_mutex)" "$BRIDGE_CPP" "video path must use locked send helper"
assert_contains_fixed "std::lock_guard<std::mutex> send_lock(*send_mutex);" "$BRIDGE_CPP" "socket writes should be serialized via send mutex"
assert_contains_fixed "&send_mutex);" "$BRIDGE_CPP" "telemetry loop should receive per-client send mutex"
assert_contains_fixed 'telemetry_loop' "$BRIDGE_CPP" 'direct ui socket telemetry loop helper missing'
assert_contains_fixed 'UI_SOCKET_PREFERRED' "$BRIDGE_CPP" 'direct ui socket preference marker missing'
assert_not_contains_fixed 'telemetry_poller->poll(0)' "$BRIDGE_CPP" 'legacy telemetry subscriber poller should be removed from bridge runtime'
assert_not_contains_fixed 'SubSocket::create(ctx, service_name, "127.0.0.1", true, true, segment_size);' "$BRIDGE_CPP" 'legacy direct-v2 telemetry subscribers should be removed'
assert_not_contains_fixed 'build_telemetry_json' "$BRIDGE_CPP" 'legacy telemetry JSON builder should be removed from bridge runtime'
assert_not_contains_fixed 'encode_car_state_typed' "$BRIDGE_CPP" 'legacy typed telemetry encoder helpers should be removed'
assert_not_contains_fixed 'send_meta_json' "$BRIDGE_CPP" 'legacy json emitter helper should be removed'

echo 'PASS: raw-only runtime contract checks passed'
