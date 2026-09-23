#!/usr/bin/env bash
set -euo pipefail

action="${1:-}"
route="${2:-}"
prod_root="${COMMAVIEW_BENCH_OP_ROOT:-/data/openpilot}"
replay_root="${COMMAVIEW_BENCH_REPLAY_ROOT:-/data/openpilot-dev}"
results="${COMMAVIEW_BENCH_RESULTS:-/data/commaview-bench/results/native-pipeline}"
sessions=(cv-sensord cv-loggerd cv-encoderd cv-camerad cv-replay cv-ui)
video_services="narrowRoadEncodeData,wideRoadEncodeData,cabinEncodeData,livestreamNarrowRoadEncodeData,livestreamWideRoadEncodeData,livestreamCabinEncodeData"

usage() {
  echo "Usage: comma4-native-pipeline.sh start <dongle|route> | stop | status" >&2
}

param() { tr -d '\000\r\n' < "/data/params/d/$1" 2>/dev/null || true; }

stop_sessions() {
  for session in "${sessions[@]}"; do
    tmux kill-session -t "$session" 2>/dev/null || true
  done
  pkill -INT -f '[c]amerad|[e]ncoderd|[l]oggerd|openpilot.system.sensord.sensord|[r]eplay|[s]elfdrive.ui.ui' 2>/dev/null || true
}

case "$action" in
  start)
    [[ -n "$route" && "$route" == *'|'* ]] || { usage; exit 2; }
    [[ "$(param IsOffroad)" == "1" ]] || { echo "ERROR: device must be offroad before startup" >&2; exit 42; }
    [[ "$(param IsEngaged)" != "1" ]] || { echo "ERROR: device is engaged" >&2; exit 42; }
    [[ -x "$prod_root/openpilot/system/camerad/camerad" ]] || { echo "ERROR: production camerad missing" >&2; exit 1; }
    [[ -x "$replay_root/openpilot/tools/replay/replay" ]] || { echo "ERROR: replay binary missing" >&2; exit 1; }

    tmux send-keys -t comma C-c 2>/dev/null || pkill -INT -f '[s]elfdrive.manager.manager' 2>/dev/null || true
    sleep 8
    stop_sessions
    mkdir -p "$results"

    tmux new-session -d -s cv-sensord "cd '$prod_root' && /usr/local/venv/bin/python -m openpilot.system.sensord.sensord 2>&1 | tee '$results/sensord.log'"
    tmux new-session -d -s cv-loggerd "cd '$prod_root' && LOGGERD_TEST=1 LOGGERD_SEGMENT_LENGTH=30 ./openpilot/system/loggerd/loggerd 2>&1 | tee '$results/loggerd.log'"
    tmux new-session -d -s cv-replay "cd '$replay_root' && . .venv/bin/activate && ./openpilot/tools/replay/replay '$route' --data_dir /data/media/0/realdata -x 1 --no-vipc -b '$video_services'"
    sleep 2
    tmux new-session -d -s cv-encoderd "cd '$prod_root' && LOGGERD_TEST=1 LOGGERD_SEGMENT_LENGTH=30 ./openpilot/system/loggerd/encoderd 2>&1 | tee '$results/encoderd.log'"
    tmux new-session -d -s cv-camerad "cd '$prod_root' && ./openpilot/system/camerad/camerad 2>&1 | tee '$results/camerad.log'"
    sleep 10
    printf 1 > /data/params/d/IsOnroad
    printf 0 > /data/params/d/IsOffroad
    printf 0 > /data/params/d/IsEngaged
    tmux new-session -d -s cv-ui "cd '$prod_root' && /usr/local/venv/bin/python -m openpilot.selfdrive.ui.ui"
    bash /data/commaview/stop.sh >/dev/null
    COMMAVIEWD_RESTART_REASON=native-pipeline-preflight bash /data/commaview/start.sh >/dev/null
    "$0" status
    ;;
  stop)
    stop_sessions
    printf 0 > /data/params/d/IsOnroad
    printf 1 > /data/params/d/IsOffroad
    printf 0 > /data/params/d/IsEngaged
    echo "STOPPED: native pipeline; reboot required to restore stock manager/UI"
    ;;
  status)
    printf 'IsOffroad=%s IsEngaged=%s\n' "$(param IsOffroad)" "$(param IsEngaged)"
    for process in camerad encoderd loggerd replay; do
      printf '%s=%s\n' "$process" "$(pgrep -c "$process" 2>/dev/null || true)"
    done
    ;;
  *) usage; exit 2 ;;
esac
