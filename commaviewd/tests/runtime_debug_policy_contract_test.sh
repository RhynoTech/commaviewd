#!/usr/bin/env bash
set -euo pipefail

fail() {
  echo "FAIL: $1"
  exit 1
}

assert_contains_fixed() {
  local needle="$1"
  local file="$2"
  local message="$3"
  grep -Fq -- "$needle" "$file" || fail "$message"
}

assert_not_contains_fixed() {
  local needle="$1"
  local file="$2"
  local message="$3"
  ! grep -Fq -- "$needle" "$file" || fail "$message"
}

runtime_debug_policy_contract_main() {
  local root
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  local bridge_cpp="$root/src/bridge_runtime.cc"
  local policy_header="$root/src/telemetry_policy.h"

  [ -f "$bridge_cpp" ] || fail "missing $bridge_cpp"
  [ -f "$policy_header" ] || fail "missing $policy_header"

  assert_contains_fixed 'enum class ServiceMode { Off, Sample, Pass };' "$policy_header" 'missing telemetry service mode enum'
  assert_contains_fixed '{"uiStateOnroad", {ServiceMode::Pass, 0}}' "$policy_header" 'uiStateOnroad telemetry should default to pass'
  assert_contains_fixed '{"selfdriveState", {ServiceMode::Pass, 0}}' "$policy_header" 'selfdriveState telemetry should default to pass'
  assert_contains_fixed '{"carState", {ServiceMode::Pass, 0}}' "$policy_header" 'carState telemetry should default to pass'
  assert_contains_fixed '{"controlsState", {ServiceMode::Pass, 0}}' "$policy_header" 'controlsState telemetry should default to pass'
  assert_contains_fixed '{"onroadEvents", {ServiceMode::Pass, 0}}' "$policy_header" 'onroadEvents telemetry should default to pass'
  assert_contains_fixed '{"driverMonitoringState", {ServiceMode::Pass, 0}}' "$policy_header" 'driverMonitoringState telemetry should default to pass'
  assert_contains_fixed '{"driverStateV2", {ServiceMode::Pass, 0}}' "$policy_header" 'driverStateV2 telemetry should default to pass'
  assert_contains_fixed '{"modelV2", {ServiceMode::Pass, 0}}' "$policy_header" 'modelV2 telemetry should default to pass'
  assert_contains_fixed '{"radarState", {ServiceMode::Pass, 0}}' "$policy_header" 'radarState telemetry should default to pass'
  assert_contains_fixed '{"liveCalibration", {ServiceMode::Pass, 0}}' "$policy_header" 'liveCalibration telemetry should default to pass'
  assert_contains_fixed '{"carOutput", {ServiceMode::Pass, 0}}' "$policy_header" 'carOutput telemetry should default to pass'
  assert_contains_fixed '{"carControl", {ServiceMode::Pass, 0}}' "$policy_header" 'carControl telemetry should default to pass'
  assert_contains_fixed '{"liveParameters", {ServiceMode::Pass, 0}}' "$policy_header" 'liveParameters telemetry should default to pass'
  assert_contains_fixed '{"longitudinalPlan", {ServiceMode::Pass, 0}}' "$policy_header" 'longitudinalPlan telemetry should default to pass'
  assert_contains_fixed '{"carParams", {ServiceMode::Pass, 0}}' "$policy_header" 'carParams telemetry should default to pass'
  assert_contains_fixed '{"deviceState", {ServiceMode::Pass, 0}}' "$policy_header" 'deviceState telemetry should default to pass'
  assert_contains_fixed '{"roadCameraState", {ServiceMode::Pass, 0}}' "$policy_header" 'roadCameraState telemetry should default to pass'
  assert_contains_fixed '{"pandaStatesSummary", {ServiceMode::Pass, 0}}' "$policy_header" 'pandaStatesSummary telemetry should default to pass'
  assert_contains_fixed '{"onroadProjection", {ServiceMode::Pass, 0}}' "$policy_header" 'onroadProjection telemetry should default to pass'
  assert_not_contains_fixed '{"commaViewControl", {ServiceMode::Pass, 0}}' "$policy_header" 'legacy commaViewControl runtime policy should be removed'
  assert_not_contains_fixed '{"commaViewScene", {ServiceMode::Pass, 0}}' "$policy_header" 'legacy commaViewScene runtime policy should be removed'
  assert_not_contains_fixed '{"commaViewStatus", {ServiceMode::Pass, 0}}' "$policy_header" 'legacy commaViewStatus runtime policy should be removed'
  assert_contains_fixed 'service_policy_subscribes' "$policy_header" 'missing service_policy_subscribes helper'
  assert_contains_fixed 'std::thread telemetry_thread' "$bridge_cpp" 'direct ui socket cutover should run telemetry in a dedicated thread'
  assert_contains_fixed 'telemetry_thread.join()' "$bridge_cpp" 'telemetry thread should be joined on disconnect'
  assert_contains_fixed 'ui_export_socket.h' "$bridge_cpp" 'direct ui socket bridge should include ui export socket support'
  assert_contains_fixed 'UI_SOCKET_PREFERRED=' "$bridge_cpp" 'bridge startup should report ui socket state'
  assert_not_contains_fixed 'SubSocket::create(ctx, service_name, "127.0.0.1", true, true, segment_size);' "$bridge_cpp" 'legacy direct-v2 telemetry subscribers should be removed from bridge runtime'
  assert_contains_fixed 'runtime_debug_stats_path()' "$bridge_cpp" 'runtime stats should still be written to the runtime stats path'

  echo 'PASS: runtime debug policy contract checks passed'
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  runtime_debug_policy_contract_main
fi
