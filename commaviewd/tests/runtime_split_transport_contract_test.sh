#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BRIDGE="$ROOT/src/bridge_runtime.cc"
POLICY="$ROOT/src/policy.cpp"

grep -q 'PORT_TELEMETRY = 8203' "$BRIDGE"
grep -q 'handle_telemetry_client' "$BRIDGE"
grep -q 'telemetry_on_video' "$BRIDGE"
grep -q 'transportVersion' "$POLICY"
grep -q 'clientRole' "$POLICY"
printf 'PASS: runtime split transport contract holds\n'
