#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE_CPP="$ROOT/src/bridge_runtime.cc"
POLICY_HEADER="$ROOT/src/telemetry_policy.h"

[ -f "$BRIDGE_CPP" ] || { echo "FAIL: missing $BRIDGE_CPP"; exit 1; }
[ -f "$POLICY_HEADER" ] || { echo "FAIL: missing $POLICY_HEADER"; exit 1; }

grep -Fq "enum class ServiceMode { Off, Sample, Pass };" "$POLICY_HEADER" || { echo "FAIL: missing telemetry service mode enum"; exit 1; }
grep -Fq "{\"carState\", {ServiceMode::Sample, 2}}" "$POLICY_HEADER" || { echo "FAIL: carState should default to SAMPLE@2Hz"; exit 1; }
grep -Fq "{\"alertDebug\", {ServiceMode::Off, 0}}" "$POLICY_HEADER" || { echo "FAIL: alertDebug should default OFF"; exit 1; }
grep -Fq "{\"modelDataV2SP\", {ServiceMode::Off, 0}}" "$POLICY_HEADER" || { echo "FAIL: modelDataV2SP should default OFF"; exit 1; }
grep -Fq "{\"longitudinalPlanSP\", {ServiceMode::Off, 0}}" "$POLICY_HEADER" || { echo "FAIL: longitudinalPlanSP should default OFF"; exit 1; }
grep -Fq "{\"deviceState\", {ServiceMode::Pass, 0}}" "$POLICY_HEADER" || { echo "FAIL: deviceState should default PASS"; exit 1; }
grep -Fq "{\"selfdriveState\", {ServiceMode::Pass, 0}}" "$POLICY_HEADER" || { echo "FAIL: selfdriveState should default PASS"; exit 1; }
grep -Fq "{\"liveCalibration\", {ServiceMode::Pass, 0}}" "$POLICY_HEADER" || { echo "FAIL: liveCalibration should default PASS"; exit 1; }
grep -Fq "{\"radarState\", {ServiceMode::Pass, 0}}" "$POLICY_HEADER" || { echo "FAIL: radarState should default PASS"; exit 1; }
grep -Fq "{\"modelV2\", {ServiceMode::Pass, 0}}" "$POLICY_HEADER" || { echo "FAIL: modelV2 should default PASS"; exit 1; }
grep -Fq "service_policy_subscribes" "$POLICY_HEADER" || { echo "FAIL: missing service_policy_subscribes helper"; exit 1; }
grep -Fq "telem_policies[i] = default_service_policy_for_name(TELEMETRY_SERVICES[i]);" "$BRIDGE_CPP" || { echo "FAIL: bridge should resolve per-service policy for each telemetry socket"; exit 1; }
grep -Fq "if (!service_policy_subscribes(telem_policies[i])) continue;" "$BRIDGE_CPP" || { echo "FAIL: OFF services should not subscribe"; exit 1; }

echo "PASS: runtime debug policy contract checks passed"
