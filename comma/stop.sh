#!/usr/bin/env bash
set +e
RUN=/data/commaview/run

commaview_pids() {
  local proc pid cmd
  for proc in /proc/[0-9]*; do
    pid="${proc##*/}"
    [ "$pid" = "$$" ] && continue
    [ -r "$proc/cmdline" ] || continue
    cmd="$(tr '\000' ' ' < "$proc/cmdline" 2>/dev/null || true)"
    case "$cmd" in
      *"/data/commaview/commaviewd bridge"*|*"/data/commaview/commaviewd control"*|*"/data/commaview/src/commaview_drive_stats.py"*)
        printf '%s\n' "$pid"
        ;;
    esac
  done | sort -nu
}

pid_alive() {
  [ -n "$1" ] && kill -0 "$1" 2>/dev/null
}

# Polls every 0.2s, up to <attempts> times, until none of <pids> is alive.
wait_for_pids_exit() {
  local attempts="$1"
  local pids="$2"
  local pid remaining
  for _ in $(seq 1 "$attempts"); do
    remaining=""
    for pid in $pids; do
      if pid_alive "$pid"; then
        remaining="$remaining $pid"
      fi
    done
    [ -z "$remaining" ] && return 0
    sleep 0.2
  done
  return 1
}

stop_pids() {
  local pids="$1"
  [ -n "$pids" ] || return 0

  # shellcheck disable=SC2086
  kill $pids 2>/dev/null || true
  wait_for_pids_exit 25 "$pids" && return 0

  # shellcheck disable=SC2086
  kill -9 $pids 2>/dev/null || true
  wait_for_pids_exit 10 "$pids" && return 0

  return 1
}

# Pid files live on /data and outlive both the processes they name and reboots
# (power loss never runs this script). A recorded pid may since have been reused
# by an openpilot process, and manager never restarts a process that dies, so
# signalling it blindly leaves openpilot with "Process Not Running" and unable to
# engage. Only signal a recorded pid that is still one of ours: a commaviewd mode
# or a start.sh subshell (supervisor / log rotation loop), never our own parent.
tracked_pid_is_runtime() {
  local pid="$1" cmd
  [ "$pid" = "$$" ] && return 1
  [ "$pid" = "$PPID" ] && return 1
  [ -r "/proc/$pid/cmdline" ] || return 1
  cmd="$(tr '\000' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
  case "$cmd" in
    *"/data/commaview/commaviewd bridge"*|*"/data/commaview/commaviewd control"*|*"/data/commaview/start.sh"*)
      return 0
      ;;
  esac
  return 1
}

tracked_pids=""
for f in bridge.pid control.pid bridge-supervisor.pid control-supervisor.pid log-rotation.pid; do
  if [ -f "$RUN/$f" ]; then
    pid="$(cat "$RUN/$f" 2>/dev/null)"
    case "$pid" in
      ''|*[!0-9]*) ;;
      *)
        if tracked_pid_is_runtime "$pid"; then
          tracked_pids="$tracked_pids $pid"
        fi
        ;;
    esac
    rm -f "$RUN/$f"
  fi
done

runtime_pids="$(commaview_pids | tr '\n' ' ')"
all_pids="$(printf '%s\n' $tracked_pids $runtime_pids 2>/dev/null | awk 'NF' | sort -nu | tr '\n' ' ')"
if ! stop_pids "$all_pids"; then
  echo "ERROR: failed to stop all CommaView runtime processes:$(commaview_pids | tr '\n' ' ')" >&2
  exit 1
fi

leftover="$(commaview_pids | tr '\n' ' ')"
if [ -n "$leftover" ]; then
  echo "ERROR: CommaView runtime processes still running:$leftover" >&2
  exit 1
fi

rm -f "$RUN/bridge.pid" "$RUN/control.pid" "$RUN/bridge-supervisor.pid" "$RUN/control-supervisor.pid" "$RUN/log-rotation.pid"
# A position from the drive log means nothing once the runtime has stopped.
rm -rf /dev/shm/commaview
echo "CommaView stopped"
