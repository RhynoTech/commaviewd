#!/usr/bin/env bash
set +e
LOG=/data/commaview/logs
RUN=/data/commaview/run
mkdir -p "$LOG" "$RUN"

# Stop stale runtime processes first
bash /data/commaview/stop.sh >/dev/null 2>&1 || true

# Launch bridge mode (video/telemetry stream)
nohup nice -n 19 /data/commaview/commaview-bridge bridge >> "$LOG/commaviewd-bridge.log" 2>&1 &
echo $! > "$RUN/bridge.pid"

# Launch control mode (API + tailscale policy)
COMMAVIEW_API_TOKEN_FILE=/data/commaview/api/auth.token \
  nohup nice -n 19 /data/commaview/commaview-bridge control >> "$LOG/commaviewd-control.log" 2>&1 &
echo $! > "$RUN/control.pid"

echo "CommaView runtime started (bridge+control)"
