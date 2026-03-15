#!/usr/bin/env bash
set -euo pipefail

if ! grep -Fq "telemetry_mode()" commaviewd/src/control_mode.cpp; then
  echo "FAIL: telemetry_mode() helper missing in control_mode.cpp"
  exit 1
fi

if ! grep -Fq "telemetryMode" commaviewd/src/control_mode.cpp; then
  echo "FAIL: /commaview/status response does not include telemetryMode"
  exit 1
fi

if ! grep -Fq "raw-only" comma4/start.sh; then
  echo "FAIL: comma4/start.sh missing default COMMAVIEWD_TELEMETRY_MODE default raw-only"
  exit 1
fi

if ! grep -Fq "telemetry_mode_label" commaviewd/src/bridge_runtime.cc; then
  echo "FAIL: bridge startup telemetry mode label missing"
  exit 1
fi

echo "PASS: commaviewd telemetry hardening guard checks passed"
