#!/usr/bin/env bash
# Runs CommaView maintenance that restarts the runtime or changes openpilot's files (install,
# update, uninstall, onroad UI export repair) only while openpilot is offroad.
#
# Asked for while the car is onroad, the job is queued instead, and a small waiter (one stat of
# a param file every POLL_SEC, nothing else) runs it once openpilot's manager is up and IsOffroad
# has read 1 for STABLE_SEC in a row. Nothing here ever asks openpilot to go offroad (no
# OffroadMode): a drive is never interrupted for CommaView.
#
# One job at a time: queueing replaces the job that was waiting. The queue lives outside the
# install directory, so an install or uninstall can replace /data/commaview under it.
#
#   run_when_offroad.sh queue ACTION [--file PATH]... -- COMMAND [ARG]...
#       Queue COMMAND (ACTION names it in the status: install, uninstall, repair). Each --file is
#       copied into the job directory; an argument starting with @JOB@ is rewritten to that
#       directory. Prints the status JSON.
#   run_when_offroad.sh resume     start the waiter for a queued job if none is running (start.sh)
#   run_when_offroad.sh status     the status JSON ({"state":"none"} when nothing was queued)
#   run_when_offroad.sh cancel     drop the queued job and stop its waiter
#   run_when_offroad.sh wait       the waiter itself (started by queue/resume)
set +e

STATE_DIR="${COMMAVIEWD_DEFERRED_DIR:-/data/commaview-deferred}"
PARAMS_DIR="${COMMAVIEWD_PARAMS_DIR:-/data/params/d}"
PROC_ROOT="${COMMAVIEWD_PROC_ROOT:-/proc}"
STABLE_SEC="${COMMAVIEWD_DEFERRED_OFFROAD_STABLE_SEC:-60}"
POLL_SEC="${COMMAVIEWD_DEFERRED_POLL_SEC:-10}"
REQUIRE_MANAGER="${COMMAVIEWD_DEFERRED_REQUIRE_MANAGER:-1}"
# A job that finds the car onroad again when it starts (exit 42) goes back to waiting, this often.
MAX_ATTEMPTS="${COMMAVIEWD_DEFERRED_MAX_ATTEMPTS:-5}"
JOB_DIR="$STATE_DIR/job"
RUNNER="$STATE_DIR/runner.sh"
STATUS="$STATE_DIR/status.json"
LOG="$STATE_DIR/deferred.log"
LOG_MAX_BYTES=262144

now_ms() {
  local s ns
  s="$(date +%s 2>/dev/null)"
  ns="$(date +%N 2>/dev/null)"
  case "$ns" in ''|*[!0-9]*) ns=000000000 ;; esac
  printf '%s%s\n' "${s:-0}" "${ns:0:3}"
}

json_string() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  s="${s//$'\n'/ }"
  printf '"%s"' "$s"
}

job_value() {
  [ -f "$JOB_DIR/$1" ] && tr -d '\r\n' < "$JOB_DIR/$1" 2>/dev/null
}

write_status() {
  local state="$1" exit_status="${2:-}"
  local action queued started finished attempts tmp
  action="$(job_value action)"
  queued="$(job_value queued_at_ms)"
  started="$(job_value started_at_ms)"
  finished="$(job_value finished_at_ms)"
  attempts="$(job_value attempts)"
  mkdir -p "$STATE_DIR" || return 0
  tmp="$STATUS.tmp.$$"
  {
    printf '{"state":%s,"action":%s' "$(json_string "$state")" "$(json_string "${action:-}")"
    printf ',"waitsFor":"offroad","queuedAtMs":%s' "${queued:-0}"
    printf ',"startedAtMs":%s,"finishedAtMs":%s,"attempts":%s' "${started:-0}" "${finished:-0}" "${attempts:-0}"
    [ -n "$exit_status" ] && printf ',"exitStatus":%s' "$exit_status"
    printf ',"updatedAtMs":%s}\n' "$(now_ms)"
  } > "$tmp" 2>/dev/null && mv -f "$tmp" "$STATUS" 2>/dev/null
}

log() {
  mkdir -p "$STATE_DIR" 2>/dev/null || return 0
  if [ -f "$LOG" ] && [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -gt "$LOG_MAX_BYTES" ]; then
    mv -f "$LOG" "$LOG.1" 2>/dev/null
  fi
  printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)" "$*" >> "$LOG" 2>/dev/null
}

read_param() {
  [ -f "$PARAMS_DIR/$1" ] || return 0
  tr -d '\000\r\n' < "$PARAMS_DIR/$1" 2>/dev/null
}

# 0 only when openpilot itself says offroad. A missing or unreadable IsOffroad is not offroad.
openpilot_offroad() {
  [ "$(read_param IsOffroad)" = "1" ]
}

# Before manager starts (early boot) IsOffroad still holds the last session's value.
manager_running() {
  [ "$REQUIRE_MANAGER" = "1" ] || return 0
  local proc cmd
  for proc in "$PROC_ROOT"/[0-9]*; do
    [ -r "$proc/cmdline" ] || continue
    cmd=" $(tr '\000' ' ' < "$proc/cmdline" 2>/dev/null) "
    case "$cmd" in
      *" ./manager.py "*|*"/system/manager/manager.py "*|*"system.manager.manager "*|*"/selfdrive/manager/manager.py "*|*"selfdrive.manager.manager "*)
        return 0 ;;
    esac
  done
  return 1
}

waiter_pid() {
  local pid cmd
  pid="$(tr -d '\r\n' < "$STATE_DIR/waiter.pid" 2>/dev/null)"
  case "$pid" in ''|*[!0-9]*) return 1 ;; esac
  [ -r "$PROC_ROOT/$pid/cmdline" ] || return 1
  cmd="$(tr '\000' ' ' < "$PROC_ROOT/$pid/cmdline" 2>/dev/null)"
  case "$cmd" in
    *"$RUNNER wait"*) printf '%s\n' "$pid"; return 0 ;;
  esac
  return 1
}

start_waiter() {
  waiter_pid >/dev/null && return 0
  [ -f "$JOB_DIR/args" ] || return 0
  if command -v setsid >/dev/null 2>&1; then
    nohup setsid bash "$RUNNER" wait </dev/null >>"$LOG" 2>&1 &
  else
    nohup bash "$RUNNER" wait </dev/null >>"$LOG" 2>&1 &
  fi
  echo $! > "$STATE_DIR/waiter.pid"
}

# A job that has started is left to finish: killing an install halfway would be worse.
job_running() {
  waiter_pid >/dev/null && [ -f "$JOB_DIR/started_at_ms" ] && [ ! -f "$JOB_DIR/finished_at_ms" ]
}

stop_waiter() {
  local pid
  pid="$(waiter_pid)" || return 0
  [ "$pid" = "$$" ] && return 0
  kill "$pid" 2>/dev/null
}

cmd_queue() {
  local action="$1"
  shift
  local files=()
  while [ "$#" -gt 0 ]; do
    case "$1" in
      --file) files+=("$2"); shift 2 ;;
      --) shift; break ;;
      *) echo "ERROR: unexpected argument: $1" >&2; return 2 ;;
    esac
  done
  [ -n "$action" ] && [ "$#" -gt 0 ] || { echo "ERROR: queue needs an action and a command" >&2; return 2; }

  mkdir -p "$STATE_DIR" || return 1
  if job_running; then
    echo "ERROR: a deferred $(job_value action) is running now; try again when it has finished" >&2
    cmd_status
    return 3
  fi
  stop_waiter
  rm -rf "$JOB_DIR"
  mkdir -p "$JOB_DIR" || return 1
  # The waiter runs from its own copy: an install or uninstall replaces the scripts it came from.
  if [ "$(readlink -f "${BASH_SOURCE[0]}")" != "$(readlink -f "$RUNNER" 2>/dev/null)" ]; then
    cp -f "${BASH_SOURCE[0]}" "$RUNNER.tmp" && mv -f "$RUNNER.tmp" "$RUNNER" || return 1
  fi
  local f
  for f in "${files[@]}"; do
    cp -f "$f" "$JOB_DIR/$(basename "$f")" || return 1
  done
  local arg
  : > "$JOB_DIR/args"
  for arg in "$@"; do
    case "$arg" in
      @JOB@*) arg="$JOB_DIR${arg#@JOB@}" ;;
    esac
    printf '%s\0' "$arg" >> "$JOB_DIR/args"
  done
  printf '%s\n' "$action" > "$JOB_DIR/action"
  now_ms > "$JOB_DIR/queued_at_ms"
  echo 0 > "$JOB_DIR/attempts"
  write_status waiting
  log "queued $action: $*"
  start_waiter
  cat "$STATUS"
}

cmd_wait() {
  echo "$$" > "$STATE_DIR/waiter.pid"
  local stable=0 attempts ec action
  local -a args=()
  while [ -f "$JOB_DIR/args" ]; do
    if manager_running && openpilot_offroad; then
      stable=$((stable + POLL_SEC))
    else
      stable=0
    fi
    if [ "$stable" -lt "$STABLE_SEC" ]; then
      sleep "$POLL_SEC"
      continue
    fi
    mapfile -t -d '' args < "$JOB_DIR/args"
    action="$(job_value action)"
    attempts="$(job_value attempts)"
    attempts=$(( ${attempts:-0} + 1 ))
    echo "$attempts" > "$JOB_DIR/attempts"
    now_ms > "$JOB_DIR/started_at_ms"
    write_status running
    log "running $action (attempt $attempts): ${args[*]}"
    ( cd / && COMMAVIEWD_DEFERRED_JOB=1 "${args[@]}" ) >> "$LOG" 2>&1
    ec=$?
    now_ms > "$JOB_DIR/finished_at_ms"
    if [ "$ec" = "42" ] && [ "$attempts" -lt "$MAX_ATTEMPTS" ]; then
      # The car went onroad again just as it started: wait for the next offroad stretch.
      log "$action found the car onroad (exit 42); waiting again"
      write_status waiting "$ec"
      rm -f "$JOB_DIR/started_at_ms" "$JOB_DIR/finished_at_ms"
      stable=0
      continue
    fi
    if [ "$ec" = "0" ]; then
      write_status done "$ec"
    else
      write_status failed "$ec"
    fi
    log "$action finished: exit $ec"
    rm -rf "$JOB_DIR"
    rm -f "$STATE_DIR/waiter.pid"
    if [ "$action" = "uninstall" ] && [ "$ec" = "0" ]; then
      # Nothing of CommaView stays behind an uninstall, this queue included.
      rm -rf "$STATE_DIR"
    fi
    return "$ec"
  done
  rm -f "$STATE_DIR/waiter.pid"
}

cmd_status() {
  if [ -f "$STATUS" ]; then
    cat "$STATUS"
  else
    printf '{"state":"none"}\n'
  fi
}

cmd_cancel() {
  if job_running; then
    echo "ERROR: the deferred $(job_value action) is running now and can't be cancelled" >&2
    return 3
  fi
  stop_waiter
  if [ -d "$JOB_DIR" ]; then
    log "cancelled $(job_value action)"
    write_status cancelled
    rm -rf "$JOB_DIR"
  fi
  rm -f "$STATE_DIR/waiter.pid"
}

case "${1:-}" in
  queue) shift; cmd_queue "$@" ;;
  resume) start_waiter ;;
  status) cmd_status ;;
  cancel) cmd_cancel ;;
  wait) cmd_wait ;;
  *) echo "Usage: run_when_offroad.sh queue ACTION [--file PATH]... -- COMMAND [ARG]... | resume | status | cancel" >&2; exit 2 ;;
esac
