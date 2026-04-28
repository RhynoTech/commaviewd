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
ONROAD_UI_EXPORT_VERIFY="$INSTALL_DIR/scripts/verify_onroad_ui_export_patch.sh"
ONROAD_UI_EXPORT_APPLY="$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh"
ONROAD_UI_EXPORT_LOG="$LOG_DIR/onroad-ui-export-startup.log"
ONROAD_UI_EXPORT_RESTART_MARKER="$RUN_DIR/onroad-ui-export-ui-restart-needed"

mkdir -p "$LOG_DIR" "$RUN_DIR" "$CONFIG_DIR"
echo "$RESTART_REASON" > "$RUN_DIR/last-restart-reason.txt"

if [ ! -s "$RUNTIME_DEBUG_CONFIG" ]; then
  if [ -s "$DEFAULTS_FILE" ]; then
    cp "$DEFAULTS_FILE" "$RUNTIME_DEBUG_CONFIG"
  else
    printf "%s\n" '{"configVersion":1,"instrumentationLevel":"standard","services":{"uiStateOnroad":{"mode":"pass"},"selfdriveState":{"mode":"pass"},"carState":{"mode":"pass"},"controlsState":{"mode":"pass"},"onroadEvents":{"mode":"pass"},"driverMonitoringState":{"mode":"pass"},"driverStateV2":{"mode":"pass"},"modelV2":{"mode":"pass"},"radarState":{"mode":"pass"},"liveCalibration":{"mode":"pass"},"carOutput":{"mode":"pass"},"carControl":{"mode":"pass"},"liveParameters":{"mode":"pass"},"longitudinalPlan":{"mode":"pass"},"carParams":{"mode":"pass"},"deviceState":{"mode":"pass"},"roadCameraState":{"mode":"pass"},"pandaStatesSummary":{"mode":"pass"}}}' > "$RUNTIME_DEBUG_CONFIG"
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

restart_openpilot_ui_if_pending() {
  if [ ! -f "$ONROAD_UI_EXPORT_RESTART_MARKER" ]; then
    return 0
  fi

  is_onroad="$(cat /data/params/d/IsOnroad 2>/dev/null | tr -d "\000\r\n" || echo 0)"
  if [ "$is_onroad" = "1" ]; then
    echo "WARN: deferred onroad UI export restart still pending while onroad" >> "$ONROAD_UI_EXPORT_LOG"
    return 0
  fi

  if ! command -v pkill >/dev/null 2>&1; then
    echo "WARN: pkill unavailable; deferred onroad UI export restart remains pending" >> "$ONROAD_UI_EXPORT_LOG"
    return 0
  fi

  if command -v pgrep >/dev/null 2>&1 && ! pgrep -f "selfdrive.ui.ui" >/dev/null 2>&1; then
    echo "INFO: openpilot UI not running; clearing deferred onroad UI export restart" >> "$ONROAD_UI_EXPORT_LOG"
    rm -f "$ONROAD_UI_EXPORT_RESTART_MARKER"
    return 0
  fi

  echo "INFO: consuming deferred onroad UI export restart" >> "$ONROAD_UI_EXPORT_LOG"
  pkill -INT -f "selfdrive.ui.ui" 2>/dev/null || true
  sleep 2
  rm -f "$ONROAD_UI_EXPORT_RESTART_MARKER"
}

refresh_onroad_ui_export_status() {
  if [ ! -x "$ONROAD_UI_EXPORT_VERIFY" ]; then
    return 0
  fi

  if "$ONROAD_UI_EXPORT_VERIFY" --json >> "$ONROAD_UI_EXPORT_LOG" 2>&1; then
    echo "INFO: onroad UI export patch verified at startup" >> "$ONROAD_UI_EXPORT_LOG"
    return 0
  fi

  is_onroad="$(cat /data/params/d/IsOnroad 2>/dev/null | tr -d "\000\r\n" || echo 0)"
  if [ "$is_onroad" = "1" ]; then
    echo "WARN: onroad UI export verify failed while onroad; skipping startup repair" >> "$ONROAD_UI_EXPORT_LOG"
    return 0
  fi

  if [ ! -x "$ONROAD_UI_EXPORT_APPLY" ]; then
    echo "WARN: onroad UI export verify failed and repair helper is missing" >> "$ONROAD_UI_EXPORT_LOG"
    return 0
  fi

  if "$ONROAD_UI_EXPORT_APPLY" >> "$ONROAD_UI_EXPORT_LOG" 2>&1; then
    echo "INFO: onroad UI export patch repaired at startup" >> "$ONROAD_UI_EXPORT_LOG"
  else
    echo "WARN: onroad UI export startup repair failed" >> "$ONROAD_UI_EXPORT_LOG"
  fi
}

refresh_onroad_ui_export_status
restart_openpilot_ui_if_pending

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

# Launch control mode (API + pairing/runtime status)
COMMAVIEWD_API_TOKEN_FILE=/data/commaview/api/auth.token \
COMMAVIEWD_RUNTIME_DEBUG_CONFIG="$RUNTIME_DEBUG_CONFIG" \
COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS="$DEFAULTS_FILE" \
COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE="$RUNTIME_DEBUG_EFFECTIVE" \
COMMAVIEWD_RUNTIME_STATS="$RUNTIME_DEBUG_STATS" \
COMMAVIEWD_RESTART_REASON="$RESTART_REASON" \
nohup nice -n 19 /data/commaview/commaviewd control >> "$LOG_DIR/commaviewd-control.log" 2>&1 &
echo $! > "$RUN_DIR/control.pid"

echo "CommaView runtime started (bridge+control)"
