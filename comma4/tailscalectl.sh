#!/usr/bin/env bash
set -euo pipefail

PARAMS_DIR="${COMMAVIEW_PARAMS_DIR:-/data/params/d}"
TAILSCALE_DIR="${COMMAVIEW_TAILSCALE_DIR:-/data/commaview/tailscale}"
TAILSCALE_BIN="${COMMAVIEW_TAILSCALE_BIN:-$TAILSCALE_DIR/bin/tailscale}"
TAILSCALED_BIN="${COMMAVIEW_TAILSCALED_BIN:-$TAILSCALE_DIR/bin/tailscaled}"
SOCKET_PATH="${COMMAVIEW_TAILSCALE_SOCKET:-$TAILSCALE_DIR/state/tailscaled.sock}"
AUTHKEY_FILE="${COMMAVIEW_TAILSCALE_AUTHKEY_FILE:-$TAILSCALE_DIR/authkey}"

DISABLE_SUDO="${COMMAVIEW_DISABLE_SUDO:-0}"
USE_SUDO=0
if [ "$DISABLE_SUDO" != "1" ] && command -v sudo >/dev/null 2>&1 && sudo -n true >/dev/null 2>&1; then
  USE_SUDO=1
fi

run_cmd() {
  if [ "$USE_SUDO" = "1" ]; then
    sudo -n "$@"
  else
    "$@"
  fi
}

read_param() {
  local key="$1"
  local path="$PARAMS_DIR/$key"
  if [ -f "$path" ]; then
    tr -d '\000\r\n' < "$path"
  else
    echo ""
  fi
}

is_onroad() {
  [ "$(read_param IsOnroad)" = "1" ]
}

write_param() {
  local key="$1"
  local value="$2"
  mkdir -p "$PARAMS_DIR"
  printf "%s" "$value" > "$PARAMS_DIR/$key"
}

tailscaled_running() {
  pgrep -f "$TAILSCALED_BIN" >/dev/null 2>&1
}

authkey_pending() {
  [ -s "$AUTHKEY_FILE" ]
}

tailscale_backend_state() {
  if [ -x "$TAILSCALE_BIN" ]; then
    run_cmd "$TAILSCALE_BIN" --socket "$SOCKET_PATH" status --json 2>/dev/null \
      | python3 -c 'import json,sys; print(json.load(sys.stdin).get("BackendState","unknown"))' 2>/dev/null \
      || echo "unknown"
  else
    echo "missing"
  fi
}

status_json() {
  local enabled onroad daemon daemon_py backend auth_pending auth_pending_py
  enabled="$(read_param CommaViewTailscaleEnabled)"
  onroad="$(read_param IsOnroad)"
  daemon="false"
  if tailscaled_running; then daemon="true"; fi
  daemon_py="False"
  if [ "$daemon" = "true" ]; then daemon_py="True"; fi
  backend="$(tailscale_backend_state)"
  auth_pending="false"
  if authkey_pending; then auth_pending="true"; fi
  auth_pending_py="False"
  if [ "$auth_pending" = "true" ]; then auth_pending_py="True"; fi

  python3 - <<PY
import json
print(json.dumps({
  "enabled": "${enabled:-0}" == "1",
  "onroad": "${onroad:-0}" == "1",
  "daemonRunning": ${daemon_py},
  "backendState": "${backend}",
  "authKeyPending": ${auth_pending_py}
}))
PY
}

status_human() {
  local enabled onroad backend auth_pending
  enabled="$(read_param CommaViewTailscaleEnabled)"
  onroad="$(read_param IsOnroad)"
  backend="$(tailscale_backend_state)"
  auth_pending="0"
  if authkey_pending; then auth_pending="1"; fi
  echo "enabled=${enabled:-0}"
  echo "onroad=${onroad:-0}"
  echo "backend_state=${backend}"
  echo "auth_key_pending=${auth_pending}"
}

set_auth_key() {
  local key="$1"
  if [ -z "$key" ]; then
    echo "ERROR: auth key required" >&2
    return 1
  fi
  mkdir -p "$(dirname "$AUTHKEY_FILE")"
  umask 077
  printf "%s" "$key" > "$AUTHKEY_FILE"
}

usage() {
  cat <<USAGE
Usage: tailscalectl.sh <status|enable|disable|set-auth-key> [args] [--json]

Commands:
  status
  enable
  disable
  set-auth-key <key>
USAGE
}

main() {
  local cmd="${1:-status}"
  local authkey_arg=""
  local json_mode="0"

  shift || true

  case "$cmd" in
    set-auth-key)
      authkey_arg="${1:-}"
      if [ -z "$authkey_arg" ]; then
        echo "ERROR: set-auth-key requires a key" >&2
        usage
        exit 1
      fi
      shift || true
      ;;
    status|enable|disable)
      ;;
    *)
      usage
      exit 1
      ;;
  esac

  while [ "$#" -gt 0 ]; do
    case "$1" in
      --json) json_mode="1" ;;
      *)
        echo "Unknown option: $1" >&2
        usage
        exit 1
        ;;
    esac
    shift
  done

  case "$cmd" in
    enable)
      if is_onroad; then
        echo "ERROR: cannot enable tailscale while onroad" >&2
        exit 2
      fi
      write_param CommaViewTailscaleEnabled 1
      ;;
    disable)
      write_param CommaViewTailscaleEnabled 0
      ;;
    set-auth-key)
      set_auth_key "$authkey_arg"
      ;;
    status)
      ;;
  esac

  if [ "$json_mode" = "1" ]; then
    status_json
  else
    status_human
  fi
}

main "$@"
