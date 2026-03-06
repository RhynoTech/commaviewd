#!/usr/bin/env bash
set -euo pipefail

LOG_DIR=/data/commaview/logs
RUN_DIR=/data/commaview/run
TAILSCALE_BIN=/data/commaview/tailscale/bin/tailscale
TAILSCALED_BIN=/data/commaview/tailscale/bin/tailscaled
TAILSCALE_SOCKET=/data/commaview/tailscale/state/tailscaled.sock
TAILSCALE_STATE_FILE=/data/commaview/tailscale/state/tailscaled.state
TAILSCALE_CHECK_OFFROAD_SEC=15

BRIDGE_BACKOFF_SEQUENCE=(1 2 4 8)
BRIDGE_BACKOFF_MAX_SEC=8
BRIDGE_HEALTHY_RESET_SEC=30
bridge_backoff_index=0
bridge_backoff_sec=0
bridge_last_start_epoch=0

tailscale_next_offroad_check_epoch=0

mkdir -p "$LOG_DIR" "$RUN_DIR"

log() {
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" >> "$LOG_DIR/supervisor.log"
}

read_param() {
  local key="$1"
  local path="/data/params/d/$key"
  if [ -f "$path" ]; then
    tr -d '\000\r\n' < "$path"
  else
    echo ""
  fi
}

is_onroad() {
  [ "$(read_param IsOnroad)" = "1" ]
}

is_tailscale_enabled() {
  [ "$(read_param CommaViewTailscaleEnabled)" = "1" ]
}

is_running_pidfile() {
  local name="$1"
  local pidf="$RUN_DIR/${name}.pid"
  [ -f "$pidf" ] || return 1
  local pid
  pid="$(cat "$pidf" 2>/dev/null || true)"
  [ -n "$pid" ] || return 1
  kill -0 "$pid" 2>/dev/null
}

stop_pidfile() {
  local name="$1"
  local pidf="$RUN_DIR/${name}.pid"
  [ -f "$pidf" ] || return 0
  local pid
  pid="$(cat "$pidf" 2>/dev/null || true)"
  if [ -n "$pid" ]; then
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 20); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.2
    done
    kill -9 "$pid" 2>/dev/null || true
  fi
  rm -f "$pidf"
}

start_bg() {
  local name="$1"; shift
  local logfile="$LOG_DIR/${name}.log"
  nohup "$@" >> "$logfile" 2>&1 &
  echo $! > "$RUN_DIR/${name}.pid"
}

reset_bridge_backoff() {
  bridge_backoff_index=0
  bridge_backoff_sec=0
}

set_next_bridge_backoff() {
  bridge_backoff_sec="${BRIDGE_BACKOFF_SEQUENCE[$bridge_backoff_index]}"
  if [ "$bridge_backoff_sec" -gt "$BRIDGE_BACKOFF_MAX_SEC" ]; then
    bridge_backoff_sec="$BRIDGE_BACKOFF_MAX_SEC"
  fi
  if [ "$bridge_backoff_index" -lt "$(( ${#BRIDGE_BACKOFF_SEQUENCE[@]} - 1 ))" ]; then
    bridge_backoff_index=$((bridge_backoff_index + 1))
  fi
}

ensure_bridge_running_prod() {
  if is_running_pidfile bridge; then
    local pid cmd now arg_count
    pid="$(cat "$RUN_DIR/bridge.pid" 2>/dev/null || true)"
    cmd="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
    arg_count="$(tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null | sed '/^$/d' | wc -l | tr -d ' ')"
    if ! echo "$cmd" | grep -Fq -- '/data/commaview/commaview-bridge'; then
      log "bridge pidfile command mismatch; restarting"
      stop_pidfile bridge
    elif [ "${arg_count:-0}" -gt 1 ]; then
      log "bridge has unexpected extra arguments; restarting prod-only"
      stop_pidfile bridge
    else
      now="$(date +%s)"
      if [ "$bridge_last_start_epoch" -gt 0 ] && [ $((now - bridge_last_start_epoch)) -ge "$BRIDGE_HEALTHY_RESET_SEC" ] && { [ "$bridge_backoff_sec" -ne 0 ] || [ "$bridge_backoff_index" -ne 0 ]; }; then
        log "bridge healthy for ${BRIDGE_HEALTHY_RESET_SEC}s; resetting restart backoff"
        reset_bridge_backoff
      fi
      return 0
    fi
  fi

  if [ "$bridge_backoff_sec" -gt 0 ]; then
    log "bridge restart backoff: sleeping ${bridge_backoff_sec}s"
    sleep "$bridge_backoff_sec"
  fi

  cd /data/openpilot
  log "starting bridge (prod-only watchdog)"
  start_bg bridge nice -n 19 /data/commaview/commaview-bridge
  bridge_last_start_epoch="$(date +%s)"
  set_next_bridge_backoff
}

ensure_tailscaled_running() {
  [ -x "$TAILSCALED_BIN" ] || return 0
  if is_running_pidfile tailscaled; then
    return 0
  fi
  mkdir -p "$(dirname "$TAILSCALE_SOCKET")"
  log "starting tailscaled"
  start_bg tailscaled nice -n 19 "$TAILSCALED_BIN" --state "$TAILSCALE_STATE_FILE" --socket "$TAILSCALE_SOCKET"
}

force_tailscale_down_and_stop() {
  if command -v tailscale >/dev/null 2>&1; then
    tailscale --socket /data/commaview/tailscale/state/tailscaled.sock down >/dev/null 2>&1 || true
  fi
  if [ -x "$TAILSCALE_BIN" ]; then
    "$TAILSCALE_BIN" --socket "$TAILSCALE_SOCKET" down >/dev/null 2>&1 || true
  fi
  stop_pidfile tailscaled
  pkill -f '/data/commaview/tailscale/bin/tailscaled' 2>/dev/null || true
}

ensure_tailscale_up() {
  [ -x "$TAILSCALE_BIN" ] || return 0
  if ! "$TAILSCALE_BIN" --socket "$TAILSCALE_SOCKET" status --json 2>/dev/null | grep -q '"BackendState":"Running"'; then
    "$TAILSCALE_BIN" --socket "$TAILSCALE_SOCKET" up >/dev/null 2>&1 || true
  fi
}

ensure_tailscale_policy_offroad() {
  local now
  now="$(date +%s)"

  if [ "$now" -lt "$tailscale_next_offroad_check_epoch" ]; then
    return 0
  fi
  tailscale_next_offroad_check_epoch=$((now + TAILSCALE_CHECK_OFFROAD_SEC))

  if is_tailscale_enabled; then
    log "tailscale policy: offroad+enabled -> ensure running"
    ensure_tailscaled_running
    ensure_tailscale_up
  else
    log "tailscale policy: offroad+disabled -> force down"
    force_tailscale_down_and_stop
  fi
}

on_mode_transition() {
  local mode="$1"
  tailscale_next_offroad_check_epoch=0

  if [ "$mode" = "onroad" ]; then
    log "tailscale policy: onroad-enter -> force down"
    force_tailscale_down_and_stop
  else
    log "tailscale policy: offroad-enter -> evaluate"
    ensure_tailscale_policy_offroad
  fi
}

log "supervisor start"
prev_mode=""
while true; do
  if is_onroad; then
    mode="onroad"
  else
    mode="offroad"
  fi

  if [ "$mode" != "$prev_mode" ]; then
    log "mode switch: ${prev_mode:-none} -> $mode"
    prev_mode="$mode"
    tailscale_next_offroad_check_epoch=0
  fi

  ensure_bridge_running_prod
  if [ "$mode" = "offroad" ]; then
    ensure_tailscale_policy_offroad
  fi
  sleep 1
done
