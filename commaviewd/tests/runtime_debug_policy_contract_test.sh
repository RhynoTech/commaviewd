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

assert_carstate_sampled_runtime_contract() {
  local bridge_cpp="$1"
  python3 - "$bridge_cpp" <<'PY'
import pathlib
import re
import sys

src = pathlib.Path(sys.argv[1]).read_text()


def fail(message: str) -> None:
    print(f"FAIL: {message}")
    raise SystemExit(1)


subscribe_block = re.search(
    r'if \(include_telemetry\) \{\s*for \(int i = 0; i < NUM_TELEM; i\+\+\) \{.*?\n\s*\}\n\s*\}',
    src,
    re.S,
)
if not subscribe_block:
    fail("missing telemetry subscribe setup block")

subscribe = subscribe_block.group(0)
if re.search(r'const bool conflate\s*=\s*!\s*service_policy_samples\(telem_policies\[i\]\)\s*;', subscribe):
    fail("carState SAMPLE subscribe path should not use generic-only conflate logic")
if "conflate" not in subscribe:
    fail("missing telemetry subscribe conflate calculation")
if not re.search(r'car_state|carState', subscribe):
    fail("carState SAMPLE subscribe path should have a dedicated special-case condition")

poll_block = re.search(
    r'for \(auto\* sock : telem_ready\) \{.*?std::unique_ptr<Message> raw_msg',
    src,
    re.S,
)
if not poll_block:
    fail("missing telemetry drain/send poll loop")

poll = poll_block.group(0)
if not re.search(
    r'if\s*\((?=[^)]*service_policy_samples\(telem_policies\[telem_sock_idx\]\))(?=[^)]*(?:car_state|carState))[^)]*\)',
    poll,
):
    fail("carState SAMPLE path should use a dedicated latest-only drain branch")
if not re.search(r'while\s*\(\s*true\s*\)\s*\{(?s:.*?)receive\s*\(\s*true\s*\)(?s:.*?)\.assign\s*\(', poll):
    fail("carState SAMPLE path should retain the latest drained payload")
PY
}

runtime_debug_policy_contract_main() {
  local root
  root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
  local bridge_cpp="$root/src/bridge_runtime.cc"
  local policy_header="$root/src/telemetry_policy.h"

  [ -f "$bridge_cpp" ] || fail "missing $bridge_cpp"
  [ -f "$policy_header" ] || fail "missing $policy_header"

  assert_contains_fixed "enum class ServiceMode { Off, Sample, Pass };" "$policy_header" "missing telemetry service mode enum"
  assert_contains_fixed '{"carState", {ServiceMode::Sample, 2}}' "$policy_header" "carState should default to SAMPLE@2Hz"
  assert_contains_fixed '{"carControl", {ServiceMode::Off, 0}}' "$policy_header" "carControl should default OFF"
  assert_contains_fixed '{"carOutput", {ServiceMode::Off, 0}}' "$policy_header" "carOutput should default OFF"
  assert_contains_fixed '{"liveParameters", {ServiceMode::Off, 0}}' "$policy_header" "liveParameters should default OFF"
  assert_contains_fixed "service_policy_subscribes" "$policy_header" "missing service_policy_subscribes helper"
  assert_contains_fixed "service_policy_samples" "$policy_header" "missing service_policy_samples helper"
  assert_contains_fixed "telem_policies[i] = runtime_policy_for_index(i);" "$bridge_cpp" "bridge should resolve effective runtime policy per telemetry socket"
  assert_carstate_sampled_runtime_contract "$bridge_cpp"
  assert_contains_fixed "if (now < sampled_next_emit[i]) continue;" "$bridge_cpp" "bridge should emit SAMPLE telemetry on cadence"
  assert_contains_fixed "drainCount" "$bridge_cpp" "runtime stats should expose drainCount"
  assert_contains_fixed "runtime_debug_stats_path()" "$bridge_cpp" "runtime stats should be written to the runtime stats path"

  echo "PASS: runtime debug policy contract checks passed"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  runtime_debug_policy_contract_main
fi
