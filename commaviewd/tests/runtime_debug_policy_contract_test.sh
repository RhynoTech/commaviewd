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


def extract_brace_block(text: str, brace_start: int) -> tuple[str, int]:
    depth = 0
    for idx in range(brace_start, len(text)):
        ch = text[idx]
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
            if depth == 0:
                return text[brace_start + 1:idx], idx + 1
    fail("unterminated brace block while checking runtime contract")


def extract_block_after(marker: str) -> str:
    start = src.find(marker)
    if start < 0:
        fail(f"missing block marker: {marker}")
    brace = src.find('{', start)
    if brace < 0:
        fail(f"missing opening brace after marker: {marker}")
    body, _ = extract_brace_block(src, brace)
    return body


subscribe = extract_block_after('if (include_telemetry)')
conflate_match = re.search(r'const bool conflate\s*=\s*(.*?);', subscribe, re.S)
if not conflate_match:
    fail("missing telemetry subscribe conflate calculation")
conflate_expr = ' '.join(conflate_match.group(1).split())
if conflate_expr == '!service_policy_samples(telem_policies[i])':
    fail("carState SAMPLE subscribe path should not use generic-only conflate logic")
if '!service_policy_samples(telem_policies[i])' not in conflate_expr:
    fail("non-carState SAMPLE subscribe path should preserve existing conflate behavior")
if not re.search(r'car_state|carState', conflate_expr):
    fail("carState SAMPLE subscribe path should special-case carState")
if 'true' not in conflate_expr:
    fail("carState SAMPLE subscribe path should force conflate=true")
if 'SubSocket::create(ctx, TELEMETRY_SERVICES[i], "127.0.0.1", conflate' not in subscribe:
    fail("telemetry subscribe setup should pass the computed conflate flag into SubSocket::create")

telemetry_runtime = extract_block_after('if (include_telemetry && telemetry_poller != nullptr)')
loop_start = telemetry_runtime.find('for (auto* sock : telem_ready)')
if loop_start < 0:
    fail("missing telemetry drain/send poll loop")
loop_brace = telemetry_runtime.find('{', loop_start)
if loop_brace < 0:
    fail("missing telemetry drain/send poll loop body")
poll, _ = extract_brace_block(telemetry_runtime, loop_brace)

carstate_branch = None
for match in re.finditer(r'if\s*\((.*?)\)\s*\{', poll, re.S):
    cond = ' '.join(match.group(1).split())
    if not re.search(r'car_state|carState', cond):
        continue
    if 'service_policy_samples(telem_policies[telem_sock_idx])' not in cond and 'sample' not in cond.lower():
        continue
    brace_start = poll.find('{', match.end() - 1)
    body, block_end = extract_brace_block(poll, brace_start)
    if 'receive(true)' in body and '.assign(' in body:
        carstate_branch = (cond, body, block_end)
        break

if carstate_branch is None:
    fail("carState SAMPLE path should use a dedicated latest-only drain branch")

_, carstate_body, carstate_end = carstate_branch
if 'continue;' not in carstate_body and not re.search(
    r'\A\s*else\s+if\s*\([^)]*service_policy_samples\(telem_policies\[telem_sock_idx\]\)',
    poll[carstate_end:],
    re.S,
):
    fail("carState SAMPLE path should not fall through into the generic sampled handler")
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
