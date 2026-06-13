#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BRIDGE_CPP="$ROOT/src/bridge_runtime.cc"

source "$SCRIPT_DIR/runtime_debug_policy_contract_test.sh"

[ -f "$BRIDGE_CPP" ] || fail "missing $BRIDGE_CPP"

assert_contains_fixed 'RAW_ONLY_DEFAULT' "$BRIDGE_CPP" 'missing RAW_ONLY_DEFAULT startup marker'
assert_contains_fixed 'DIRECT_V2_UI_EXPORT_DEFAULT' "$BRIDGE_CPP" 'missing DIRECT_V2_UI_EXPORT_DEFAULT startup marker'
assert_contains_fixed '"uiStateOnroad"' "$BRIDGE_CPP" 'missing uiStateOnroad subscription'
assert_contains_fixed '"selfdriveState"' "$BRIDGE_CPP" 'missing selfdriveState subscription'
assert_contains_fixed '"carState"' "$BRIDGE_CPP" 'missing carState subscription'
assert_contains_fixed '"controlsState"' "$BRIDGE_CPP" 'missing controlsState subscription'
assert_contains_fixed '"onroadEvents"' "$BRIDGE_CPP" 'missing onroadEvents subscription'
assert_contains_fixed '"driverMonitoringState"' "$BRIDGE_CPP" 'missing driverMonitoringState subscription'
assert_contains_fixed '"driverStateV2"' "$BRIDGE_CPP" 'missing driverStateV2 subscription'
assert_contains_fixed '"modelV2"' "$BRIDGE_CPP" 'missing modelV2 subscription'
assert_contains_fixed '"radarState"' "$BRIDGE_CPP" 'missing radarState subscription'
assert_contains_fixed '"liveCalibration"' "$BRIDGE_CPP" 'missing liveCalibration subscription'
assert_contains_fixed '"carOutput"' "$BRIDGE_CPP" 'missing carOutput subscription'
assert_contains_fixed '"carControl"' "$BRIDGE_CPP" 'missing carControl subscription'
assert_contains_fixed '"liveParameters"' "$BRIDGE_CPP" 'missing liveParameters subscription'
assert_contains_fixed '"longitudinalPlan"' "$BRIDGE_CPP" 'missing longitudinalPlan subscription'
assert_contains_fixed '"carParams"' "$BRIDGE_CPP" 'missing carParams subscription'
assert_contains_fixed '"deviceState"' "$BRIDGE_CPP" 'missing deviceState subscription'
assert_contains_fixed '"roadCameraState"' "$BRIDGE_CPP" 'missing roadCameraState subscription'
assert_contains_fixed '"pandaStatesSummary"' "$BRIDGE_CPP" 'missing pandaStatesSummary subscription'
assert_contains_fixed '"onroadProjection"' "$BRIDGE_CPP" 'missing onroadProjection subscription'
assert_contains_fixed '"wideRoadCameraState"' "$BRIDGE_CPP" 'missing wideRoadCameraState subscription'
assert_contains_fixed 'std::array<const char*, 20> kTelemetryServices' "$BRIDGE_CPP" 'telemetry service table must include service indexes 18 onroadProjection and 19 wideRoadCameraState'

# The raw telemetry envelope identifies services by index, so the exact order of
# kTelemetryServices is a wire contract with the app's TelemetryClient
# (CommaView: TelemetryClient.TELEMETRY_SERVICE_TYPES). The app pins the same
# ordered list in TelemetryServiceTableContractTest; change both together.
expected_telemetry_services='uiStateOnroad
selfdriveState
carState
controlsState
onroadEvents
driverMonitoringState
driverStateV2
modelV2
radarState
liveCalibration
carOutput
carControl
liveParameters
longitudinalPlan
carParams
deviceState
roadCameraState
pandaStatesSummary
onroadProjection
wideRoadCameraState'
actual_telemetry_services="$(sed -n '/kTelemetryServices = {/,/};/p' "$BRIDGE_CPP" | grep -o '"[^"]*"' | tr -d '"')"
if [ "$actual_telemetry_services" != "$expected_telemetry_services" ]; then
  fail "kTelemetryServices order drifted from the pinned telemetry service index contract:
expected:
$expected_telemetry_services
actual:
$actual_telemetry_services"
fi
assert_not_contains_fixed '"commaViewHudLite"' "$BRIDGE_CPP" 'HUD-lite service should be gone'
assert_not_contains_fixed 'telemetry_index_for_which' "$BRIDGE_CPP" 'legacy event->service index mapping should be removed'
assert_not_contains_fixed 'car_state_idx' "$BRIDGE_CPP" 'legacy carState sample special-case should be removed'
assert_not_contains_fixed 'sampled_latest(NUM_TELEM)' "$BRIDGE_CPP" 'legacy sampled cache should be removed for HUD-lite-only path'
assert_not_contains_fixed 'sampled_have_latest(NUM_TELEM' "$BRIDGE_CPP" 'legacy sampled cache flags should be removed for HUD-lite-only path'
assert_contains_fixed "udp_video_sender.send_frame(frame, runtime_now_ns())" "$BRIDGE_CPP" "video path must use UDP sender"
assert_contains_fixed 'udp_telemetry_snapshot_loop' "$BRIDGE_CPP" 'lossy UDP telemetry snapshot pump missing'
assert_contains_fixed 'udp_telemetry_snapshot_loop, telemetry_udp_fd, PORT_TELEMETRY' "$BRIDGE_CPP" 'telemetry snapshot pump must run on a pre-bound telemetry port socket'
assert_contains_fixed 'create_udp_video_socket(PORT_TELEMETRY)' "$BRIDGE_CPP" 'UDP telemetry snapshot socket must be bound fail-closed at startup'
assert_not_contains_fixed 'telemetry_transport' "$BRIDGE_CPP" 'TCP telemetry transport negotiation should be removed'
assert_not_contains_fixed 'udp_snapshot_transport' "$BRIDGE_CPP" 'UDP snapshot TCP demotion state should be removed'
assert_not_contains_fixed 'demote_policy_for_udp_snapshot' "$BRIDGE_CPP" 'TCP telemetry demotion helper should be removed'
assert_not_contains_fixed 'handle_telemetry_client' "$BRIDGE_CPP" 'TCP telemetry client handler should be removed'
assert_contains_fixed 'UI_SOCKET_PREFERRED' "$BRIDGE_CPP" 'direct ui socket preference marker missing'
assert_not_contains_fixed 'telemetry_poller->poll(0)' "$BRIDGE_CPP" 'legacy telemetry subscriber poller should be removed from bridge runtime'
assert_not_contains_fixed 'SubSocket::create(ctx, service_name, "127.0.0.1", true, true, segment_size);' "$BRIDGE_CPP" 'legacy direct-v2 telemetry subscribers should be removed'
assert_not_contains_fixed 'build_telemetry_json' "$BRIDGE_CPP" 'legacy telemetry JSON builder should be removed from bridge runtime'
assert_not_contains_fixed 'encode_car_state_typed' "$BRIDGE_CPP" 'legacy typed telemetry encoder helpers should be removed'
assert_not_contains_fixed 'send_meta_json' "$BRIDGE_CPP" 'legacy json emitter helper should be removed'

echo 'PASS: raw-only runtime contract checks passed'
