#!/usr/bin/env bash
set -euo pipefail

SERVER_CPP="/home/pear/CommaView/bridge/cpp/src/api/http_server.cpp"
CONTROL_CPP="/home/pear/CommaView/bridge/cpp/src/runtime/control_mode.cpp"

[ -f "$SERVER_CPP" ] || { echo "FAIL: missing $SERVER_CPP"; exit 1; }
[ -f "$CONTROL_CPP" ] || { echo "FAIL: missing $CONTROL_CPP"; exit 1; }

grep -Fq '"/tailscale/status"' "$CONTROL_CPP" || { echo "FAIL: missing /tailscale/status route"; exit 1; }
grep -Fq '"/tailscale/enable"' "$CONTROL_CPP" || { echo "FAIL: missing /tailscale/enable route"; exit 1; }
grep -Fq '"/tailscale/disable"' "$CONTROL_CPP" || { echo "FAIL: missing /tailscale/disable route"; exit 1; }
grep -Fq '"/tailscale/authkey"' "$CONTROL_CPP" || { echo "FAIL: missing /tailscale/authkey route"; exit 1; }
grep -Fq 'X-CommaView-Token' "$SERVER_CPP" || { echo "FAIL: missing auth token header handling"; exit 1; }
grep -Fq 'tailscale_set_authkey' "$CONTROL_CPP" || { echo "FAIL: control mode missing tailscale_set_authkey handler"; exit 1; }

echo "PASS: control mode API contract routes present"
