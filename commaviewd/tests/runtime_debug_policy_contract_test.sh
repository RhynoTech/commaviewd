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
  assert_contains_fixed '{"commaViewHudLite", {ServiceMode::Pass, 0}}' "$policy_header" 'HUD-lite should be the sole default telemetry service'
  assert_not_contains_fixed '{"carState", {ServiceMode::Sample, 2}}' "$policy_header" 'legacy carState runtime policy should be removed'
  assert_not_contains_fixed '{"selfdriveState", {ServiceMode::Pass, 0}}' "$policy_header" 'legacy selfdriveState runtime policy should be removed'
  assert_contains_fixed 'service_policy_subscribes' "$policy_header" 'missing service_policy_subscribes helper'
  assert_contains_fixed 'std::thread telemetry_thread' "$bridge_cpp" 'HUD-lite cutover should run telemetry in a dedicated thread'
  assert_contains_fixed 'telemetry_thread.join()' "$bridge_cpp" 'telemetry thread should be joined on disconnect'
  assert_contains_fixed 'SubSocket::create(ctx, TELEMETRY_SERVICES[i], "127.0.0.1", true, true, segment_size);' "$bridge_cpp" 'HUD-lite subscriber should use conflated raw subscription'
  assert_contains_fixed 'runtime_debug_stats_path()' "$bridge_cpp" 'runtime stats should still be written to the runtime stats path'

  echo 'PASS: runtime debug policy contract checks passed'
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  runtime_debug_policy_contract_main
fi
