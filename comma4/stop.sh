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
      *"/data/commaview/commaviewd bridge"*|*"/data/commaview/commaviewd control"*)
        printf '%s\n' "$pid"
        ;;
    esac
  done | sort -nu
}

pid_alive() {
  [ -n "$1" ] && kill -0 "$1" 2>/dev/null
}

stop_pids() {
  local pids="$1"
  local pid remaining
  [ -n "$pids" ] || return 0

  # shellcheck disable=SC2086
  kill $pids 2>/dev/null || true
  for _ in $(seq 1 25); do
    remaining=""
    for pid in $pids; do
      if pid_alive "$pid"; then
        remaining="$remaining $pid"
      fi
    done
    [ -z "$remaining" ] && return 0
    sleep 0.2
  done

  # shellcheck disable=SC2086
  kill -9 $pids 2>/dev/null || true
  for _ in $(seq 1 10); do
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

tracked_pids=""
for f in bridge.pid control.pid; do
  if [ -f "$RUN/$f" ]; then
    pid="$(cat "$RUN/$f" 2>/dev/null)"
    case "$pid" in
      ''|*[!0-9]*) ;;
      *) tracked_pids="$tracked_pids $pid" ;;
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

rm -f "$RUN/bridge.pid" "$RUN/control.pid"
echo "CommaView stopped"
