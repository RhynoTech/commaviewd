#!/usr/bin/env bash
set +e

INSTALL_DIR=/data/commaview
LOG_DIR="$INSTALL_DIR/logs"
RUN_DIR="$INSTALL_DIR/run"
CONFIG_DIR="$INSTALL_DIR/config"
DEFAULTS_FILE="${COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS:-$INSTALL_DIR/runtime-debug.defaults.json}"
RUNTIME_DEBUG_CONFIG="${COMMAVIEWD_RUNTIME_DEBUG_CONFIG:-$CONFIG_DIR/runtime-debug.json}"
RUNTIME_DEBUG_EFFECTIVE="${COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE:-$RUN_DIR/runtime-debug-effective.json}"
RUNTIME_DEBUG_STATS="${COMMAVIEWD_RUNTIME_STATS:-$RUN_DIR/telemetry-stats.json}"
RESTART_REASON="${COMMAVIEWD_RESTART_REASON:-startup}"

mkdir -p "$LOG_DIR" "$RUN_DIR" "$CONFIG_DIR"
echo "$RESTART_REASON" > "$RUN_DIR/last-restart-reason.txt"

if [ ! -s "$RUNTIME_DEBUG_CONFIG" ]; then
  if [ -s "$DEFAULTS_FILE" ]; then
    cp "$DEFAULTS_FILE" "$RUNTIME_DEBUG_CONFIG"
  else
    printf "%s\n" '{"configVersion":1,"instrumentationLevel":"standard","services":{"carState":{"mode":"sample","sampleHz":2},"selfdriveState":{"mode":"pass"},"deviceState":{"mode":"pass"},"liveCalibration":{"mode":"pass"},"radarState":{"mode":"pass"},"modelV2":{"mode":"pass"},"alertDebug":{"mode":"off"},"modelDataV2SP":{"mode":"off"},"longitudinalPlanSP":{"mode":"off"},"carControl":{"mode":"off"},"carOutput":{"mode":"off"},"liveParameters":{"mode":"off"},"driverMonitoringState":{"mode":"off"},"driverStateV2":{"mode":"off"},"onroadEvents":{"mode":"off"},"roadCameraState":{"mode":"off"}}}' > "$RUNTIME_DEBUG_CONFIG"
  fi
fi

python3 - "$RUNTIME_DEBUG_CONFIG" >/dev/null 2>&1 <<'PY'
import json, sys
with open(sys.argv[1], "r", encoding="utf-8") as f:
    json.load(f)
PY
if [ $? -ne 0 ]; then
  echo "WARN: invalid runtime debug config JSON; bridge/control will fall back to safe defaults" >> "$LOG_DIR/commaviewd-control.log"
fi

# Stop stale runtime processes first
bash /data/commaview/stop.sh >/dev/null 2>&1 || true

# Launch bridge mode (video/telemetry stream)
COMMAVIEWD_RUNTIME_DEBUG_CONFIG="$RUNTIME_DEBUG_CONFIG" \
COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS="$DEFAULTS_FILE" \
COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE="$RUNTIME_DEBUG_EFFECTIVE" \
COMMAVIEWD_RUNTIME_STATS="$RUNTIME_DEBUG_STATS" \
COMMAVIEWD_RESTART_REASON="$RESTART_REASON" \
nohup nice -n 19 /data/commaview/commaviewd bridge >> "$LOG_DIR/commaviewd-bridge.log" 2>&1 &
echo $! > "$RUN_DIR/bridge.pid"

# Launch control mode (API + tailscale policy)
COMMAVIEWD_API_TOKEN_FILE=/data/commaview/api/auth.token \
COMMAVIEWD_RUNTIME_DEBUG_CONFIG="$RUNTIME_DEBUG_CONFIG" \
COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS="$DEFAULTS_FILE" \
COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE="$RUNTIME_DEBUG_EFFECTIVE" \
COMMAVIEWD_RUNTIME_STATS="$RUNTIME_DEBUG_STATS" \
COMMAVIEWD_RESTART_REASON="$RESTART_REASON" \
nohup nice -n 19 /data/commaview/commaviewd control >> "$LOG_DIR/commaviewd-control.log" 2>&1 &
echo $! > "$RUN_DIR/control.pid"

echo "CommaView runtime started (bridge+control)"
