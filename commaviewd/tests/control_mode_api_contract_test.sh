#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_CPP="$ROOT/src/http_server.cpp"
CONTROL_CPP="$ROOT/src/control_mode.cpp"
DOC_SHORT="$ROOT/docs/ai/telemetry-raw-only-readme.md"
DOC_DEEP="$ROOT/docs/ai/telemetry-raw-only-deep-dive.md"
grep -Fq "telemetryMode" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose telemetryMode"; exit 1; }
grep -Fq "persistedConfig" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose persistedConfig"; exit 1; }
grep -Fq "effectiveConfig" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose effectiveConfig"; exit 1; }
grep -Fq "runtimeStats" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose runtimeStats"; exit 1; }
grep -Fq "configVersion" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose configVersion"; exit 1; }
grep -Fq "configHash" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose configHash"; exit 1; }
grep -Fq "warnings" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose warnings"; exit 1; }
grep -Fq "safeFallback" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose safeFallback state"; exit 1; }
grep -Fq "carState" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose carState policy"; exit 1; }
grep -Fq "alertDebug" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose alertDebug policy"; exit 1; }
grep -Fq "modelDataV2SP" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose modelDataV2SP policy"; exit 1; }
grep -Fq "longitudinalPlanSP" "$CONTROL_CPP" || { echo "FAIL: /commaview/status should expose longitudinalPlanSP policy"; exit 1; }
[ -f "$SERVER_CPP" ] || { echo "FAIL: missing $SERVER_CPP"; exit 1; }
[ -f "$CONTROL_CPP" ] || { echo "FAIL: missing $CONTROL_CPP"; exit 1; }
[ -f "$DOC_SHORT" ] || { echo "FAIL: missing $DOC_SHORT"; exit 1; }
[ -f "$DOC_DEEP" ] || { echo "FAIL: missing $DOC_DEEP"; exit 1; }

grep -Fq "\"/tailscale/status\"" "$CONTROL_CPP" || { echo "FAIL: missing /tailscale/status route"; exit 1; }
grep -Fq "\"/tailscale/enable\"" "$CONTROL_CPP" || { echo "FAIL: missing /tailscale/enable route"; exit 1; }
grep -Fq "\"/tailscale/disable\"" "$CONTROL_CPP" || { echo "FAIL: missing /tailscale/disable route"; exit 1; }
grep -Fq "\"/tailscale/authkey\"" "$CONTROL_CPP" || { echo "FAIL: missing /tailscale/authkey route"; exit 1; }
grep -Fq "X-CommaView-Token" "$SERVER_CPP" || { echo "FAIL: missing auth token header handling"; exit 1; }
grep -Fq "tailscale_set_authkey" "$CONTROL_CPP" || { echo "FAIL: control mode missing tailscale_set_authkey handler"; exit 1; }

! grep -Fq "COMMAVIEWD_TELEMETRY_MODE" "$CONTROL_CPP" || { echo "FAIL: control mode should not depend on COMMAVIEWD_TELEMETRY_MODE"; exit 1; }
grep -Fq "docs/ai/telemetry-raw-only-readme.md" README.md || { echo "FAIL: missing short raw-only doc link in README"; exit 1; }
grep -Fq "docs/ai/telemetry-raw-only-deep-dive.md" README.md || { echo "FAIL: missing deep raw-only doc link in README"; exit 1; }
echo "PASS: control mode API contract routes present"
