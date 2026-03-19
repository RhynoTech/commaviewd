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

if grep -Fq "COMMAVIEWD_TELEMETRY_MODE" comma4/start.sh; then
  echo "FAIL: comma4/start.sh should not use COMMAVIEWD_TELEMETRY_MODE override anymore"
  exit 1
fi

if ! grep -Fq "commaviewd bridge" comma4/start.sh; then
  echo "FAIL: comma4/start.sh missing bridge launch command"
  exit 1
fi

if ! grep -Fq "RAW_ONLY_DEFAULT" commaviewd/src/bridge_runtime.cc; then
  echo "FAIL: bridge startup RAW_ONLY_DEFAULT marker missing"
  exit 1
fi

if grep -Fq -- "--dev" commaviewd/src/bridge_runtime.cc; then
  echo "FAIL: --dev debug flag should be absent from bridge runtime"
  exit 1
fi

if grep -Fq -- "--telem-emit-ms" commaviewd/src/bridge_runtime.cc; then
  echo "FAIL: --telem-emit-ms override flag should be absent from bridge runtime"
  exit 1
fi

if grep -Fq "COMMAVIEW_TELEMETRY_EMIT_MS" commaviewd/src/bridge_runtime.cc; then
  echo "FAIL: COMMAVIEW_TELEMETRY_EMIT_MS override env should be absent from bridge runtime"
  exit 1
fi

echo "PASS: commaviewd telemetry hardening guard checks passed"
