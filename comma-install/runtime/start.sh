#!/usr/bin/env bash
set +e
LOG=/data/commaview/logs
RUN=/data/commaview/run
mkdir -p "$LOG" "$RUN"

# Stop stale supervisor and owned children
bash /data/commaview/stop.sh >/dev/null 2>&1 || true

# Launch supervisor
nohup nice -n 19 /data/commaview/commaview-supervisor.sh >> "$LOG/supervisor.log" 2>&1 &
echo $! > "$RUN/supervisor.pid"

# Launch local API for app/status/tailscale control
if [ -f /data/commaview/api/commaview-api.py ]; then
  nohup nice -n 19 python3 /data/commaview/api/commaview-api.py >> "$LOG/commaview-api.log" 2>&1 &
  echo $! > "$RUN/commaview_api.pid"
fi

# Launch tailscale guardian (policy daemon)
echo "CommaView supervisor started"
