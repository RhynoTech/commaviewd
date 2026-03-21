#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_CPP="$ROOT/src/bridge_runtime.cc"
POLICY_HEADER="$ROOT/src/telemetry_policy.h"

[ -f "$BRIDGE_CPP" ] || { echo "FAIL: missing $BRIDGE_CPP"; exit 1; }
[ -f "$POLICY_HEADER" ] || { echo "FAIL: missing $POLICY_HEADER"; exit 1; }

grep -Fq "enum class ServiceMode { Off, Sample, Pass };" "$POLICY_HEADER" || { echo "FAIL: missing telemetry service mode enum"; exit 1; }
grep -Fq "{\"carState\", {ServiceMode::Sample, 2}}" "$POLICY_HEADER" || { echo "FAIL: carState should default to SAMPLE@2Hz"; exit 1; }
grep -Fq "{\"carControl\", {ServiceMode::Off, 0}}" "$POLICY_HEADER" || { echo "FAIL: carControl should default OFF"; exit 1; }
grep -Fq "{\"carOutput\", {ServiceMode::Off, 0}}" "$POLICY_HEADER" || { echo "FAIL: carOutput should default OFF"; exit 1; }
grep -Fq "{\"liveParameters\", {ServiceMode::Off, 0}}" "$POLICY_HEADER" || { echo "FAIL: liveParameters should default OFF"; exit 1; }
grep -Fq "service_policy_subscribes" "$POLICY_HEADER" || { echo "FAIL: missing service_policy_subscribes helper"; exit 1; }
grep -Fq "service_policy_samples" "$POLICY_HEADER" || { echo "FAIL: missing service_policy_samples helper"; exit 1; }
grep -Fq "telem_policies[i] = runtime_policy_for_index(i);" "$BRIDGE_CPP" || { echo "FAIL: bridge should resolve effective runtime policy per telemetry socket"; exit 1; }
grep -Fq 'const bool conflate = (i == car_state_idx) ? true : !service_policy_samples(telem_policies[i]);' "$BRIDGE_CPP" || { echo "FAIL: carState SAMPLE path should force conflated subscribe while other SAMPLE services keep current behavior"; exit 1; }
grep -Fq 'if (telem_sock_idx == car_state_idx && service_policy_samples(telem_policies[telem_sock_idx])) {' "$BRIDGE_CPP" || { echo "FAIL: carState SAMPLE path should use a dedicated latest-only drain branch"; exit 1; }
! grep -Fq 'if (service_policy_samples(telem_policies[telem_sock_idx])) {' "$BRIDGE_CPP" || { echo "FAIL: generic sampled-service drain branch should no longer handle carState"; exit 1; }
grep -Fq "std::unique_ptr<Message> drained(sock->receive(true));" "$BRIDGE_CPP" || { echo "FAIL: bridge should drain queued carState SAMPLE telemetry messages"; exit 1; }
grep -Fq "sampled_latest[telem_sock_idx].assign" "$BRIDGE_CPP" || { echo "FAIL: bridge should retain the latest drained SAMPLE payload"; exit 1; }
grep -Fq "if (now < sampled_next_emit[i]) continue;" "$BRIDGE_CPP" || { echo "FAIL: bridge should emit SAMPLE telemetry on cadence"; exit 1; }
grep -Fq "drainCount" "$BRIDGE_CPP" || { echo "FAIL: runtime stats should expose drainCount"; exit 1; }
grep -Fq "runtime_debug_stats_path()" "$BRIDGE_CPP" || { echo "FAIL: runtime stats should be written to the runtime stats path"; exit 1; }

echo "PASS: runtime debug policy contract checks passed"
