#!/usr/bin/env bash
set +e
RUN=/data/commaview/run

# stop runtime tracked by pidfiles
for f in bridge.pid control.pid supervisor.pid commaview_api.pid tailscaled.pid; do
  if [ -f "$RUN/$f" ]; then
    pid=$(cat "$RUN/$f" 2>/dev/null)
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 20); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.2
    done
    kill -9 "$pid" 2>/dev/null || true
    rm -f "$RUN/$f"
  fi
done

# clean old strays
pkill -f '/data/commaview/commaviewd bridge' 2>/dev/null || true
pkill -f '/data/commaview/commaviewd control' 2>/dev/null || true
pkill -f '/data/commaview/commaviewd' 2>/dev/null || true
pkill -f 'commaview-supervisor.sh' 2>/dev/null || true
pkill -f 'commaview-api.py' 2>/dev/null || true
pkill -f '/data/commaview/tailscale/bin/tailscaled' 2>/dev/null || true

if [ -x /data/commaview/tailscale/bin/tailscale ]; then
  /data/commaview/tailscale/bin/tailscale --socket /data/commaview/tailscale/state/tailscaled.sock down >/dev/null 2>&1 || true
fi

echo "CommaView stopped"
