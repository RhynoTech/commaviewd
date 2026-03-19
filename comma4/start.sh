#!/usr/bin/env bash
set +e
mkdir -p /data/commaview/logs /data/commaview/run

# Stop stale runtime processes first
bash /data/commaview/stop.sh >/dev/null 2>&1 || true

# Launch bridge mode (video/telemetry stream)
nohup nice -n 19 /data/commaview/commaviewd bridge >> /data/commaview/logs/commaviewd-bridge.log 2>&1 &
echo $! > /data/commaview/run/bridge.pid

# Launch control mode (API + tailscale policy)
COMMAVIEWD_API_TOKEN_FILE=/data/commaview/api/auth.token nohup nice -n 19 /data/commaview/commaviewd control >> /data/commaview/logs/commaviewd-control.log 2>&1 &
echo $! > /data/commaview/run/control.pid

echo "CommaView runtime started (bridge+control)"
