#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"
STATE_JSON="$INSTALL_DIR/run/onroad-ui-export-status.json"
RESTART_MARKER="$INSTALL_DIR/run/onroad-ui-export-ui-restart-needed"
PARAMS_DIR="/data/params/d"

read_param() {
  local path="$PARAMS_DIR/$1"
  [[ -f "$path" ]] || return 0
  tr -d '\000\r\n' < "$path" 2>/dev/null || true
}

request_openpilot_ui_restart() {
  mkdir -p "$(dirname "$RESTART_MARKER")"
  printf 'pending\n' > "$RESTART_MARKER"
}

restart_openpilot_ui_if_offroad() {
  local is_onroad=""

  if [ "${COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART:-0}" = "1" ]; then
    echo "INFO: skipping openpilot UI restart by request" >&2
    return 0
  fi

  is_onroad="$(read_param IsOnroad)"
  if [ "$is_onroad" = "1" ]; then
    request_openpilot_ui_restart
    echo "WARN: deferring openpilot UI restart while onroad" >&2
    return 0
  fi

  if ! command -v pkill >/dev/null 2>&1; then
    request_openpilot_ui_restart
    echo "WARN: pkill unavailable; deferring openpilot UI restart" >&2
    return 0
  fi

  if command -v pgrep >/dev/null 2>&1 && ! pgrep -f "selfdrive.ui.ui" >/dev/null 2>&1; then
    echo "INFO: openpilot UI process not running; no restart needed" >&2
    rm -f "$RESTART_MARKER"
    return 0
  fi

  echo "INFO: restarting openpilot UI to unload CommaView onroad UI export transformer output" >&2
  pkill -INT -f "selfdrive.ui.ui" 2>/dev/null || true
  sleep 2
  rm -f "$RESTART_MARKER"
}

managed_targets() {
  printf '%s\n' \
    "selfdrive/ui/commaview_export.py" \
    "selfdrive/ui/ui_state.py" \
    "selfdrive/ui/mici/onroad/augmented_road_view.py" \
    "selfdrive/ui/onroad/augmented_road_view.py"
}

backup_managed_targets() {
  local backup_root="$INSTALL_DIR/backups/onroad-ui-export-revert/$(date -u +%Y%m%d-%H%M%S)"
  local rel=""
  mkdir -p "$backup_root"
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if [ -e "$OP_ROOT/$rel" ]; then
      mkdir -p "$backup_root/$(dirname "$rel")"
      cp -a "$OP_ROOT/$rel" "$backup_root/$rel"
    fi
  done < <(managed_targets)
  printf '%s\n' "$backup_root"
}

reset_managed_targets() {
  local rel=""
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    git -C "$OP_ROOT" reset -q HEAD -- "$rel" >/dev/null 2>&1 || true
    if git -C "$OP_ROOT" ls-files --error-unmatch -- "$rel" >/dev/null 2>&1; then
      git -C "$OP_ROOT" checkout -- "$rel"
    else
      rm -f "$OP_ROOT/$rel"
    fi
  done < <(managed_targets)
}

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "WARN: upstream repo not found at $OP_ROOT; skipping onroad UI export transformer revert" >&2
  exit 0
fi

backup_root="$(backup_managed_targets)"
echo "WARN: reverting managed onroad UI export transformer targets" >&2
echo "WARN: backups written to $backup_root" >&2
reset_managed_targets
rm -f "$STATE_ENV" "$STATE_JSON" "$RESTART_MARKER"
restart_openpilot_ui_if_offroad

echo "CommaView onroad UI export transformer reverted"
