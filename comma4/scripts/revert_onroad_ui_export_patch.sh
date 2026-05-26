#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"
STATE_JSON="$INSTALL_DIR/run/onroad-ui-export-status.json"
RESTART_MARKER="$INSTALL_DIR/run/onroad-ui-export-ui-restart-needed"
BACKUP_ROOT="${COMMAVIEWD_BACKUP_ROOT:-/data/commaview-backups}"
PARAMS_DIR="${COMMAVIEWD_PARAMS_DIR:-/data/params/d}"
FORCE_OFFROAD=0
PREFLIGHT_ONLY=0
FORCE_OFFROAD_OWNED=0
FORCE_OFFROAD_PREV=""

read_param() {
  local path="$PARAMS_DIR/$1"
  [[ -f "$path" ]] || return 0
  tr -d '\000\r\n' < "$path" 2>/dev/null || true
}

write_param() {
  mkdir -p "$PARAMS_DIR"
  printf '%s' "$2" > "$PARAMS_DIR/$1"
}

restore_force_offroad_mode() {
  if [ "$FORCE_OFFROAD_OWNED" = "1" ]; then
    write_param "OffroadMode" "${FORCE_OFFROAD_PREV:-0}"
  fi
}

cleanup() {
  restore_force_offroad_mode
}

wait_until_offroad() {
  local timeout_sec="${1:-45}"
  local elapsed=0
  local is_onroad=""
  while [ "$elapsed" -lt "$timeout_sec" ]; do
    is_onroad="$(read_param IsOnroad)"
    if [ "$is_onroad" != "1" ]; then
      return 0
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done
  return 1
}

ensure_offroad_ready() {
  local is_onroad
  is_onroad="$(read_param IsOnroad)"
  if [ "$is_onroad" != "1" ]; then
    return 0
  fi

  if [ "$FORCE_OFFROAD" != "1" ]; then
    echo "ERROR: socket UI export transformer revert blocked while onroad" >&2
    exit 42
  fi

  FORCE_OFFROAD_PREV="$(read_param OffroadMode)"
  if [ "$FORCE_OFFROAD_PREV" != "1" ]; then
    echo "INFO: requesting OffroadMode for transformer revert" >&2
    write_param "OffroadMode" "1"
    FORCE_OFFROAD_OWNED=1
  fi

  echo "INFO: waiting for actual offroad transition" >&2
  if ! wait_until_offroad 45; then
    echo "ERROR: device did not transition offroad in time" >&2
    exit 42
  fi
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --force-offroad) FORCE_OFFROAD=1; shift ;;
    --preflight-only) PREFLIGHT_ONLY=1; shift ;;
    -h|--help) echo "Usage: revert_onroad_ui_export_patch.sh [--force-offroad] [--preflight-only]"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

trap cleanup EXIT
ensure_offroad_ready
if [ "$PREFLIGHT_ONLY" = "1" ]; then
  exit 0
fi

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
  local backup_parent="$BACKUP_ROOT/onroad-ui-export-revert"
  local backup_root=""
  local rel=""
  mkdir -p "$backup_parent" || return $?
  backup_root="$(mktemp -d "$backup_parent/$(date -u +%Y%m%d-%H%M%S).XXXXXX")" || return $?
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if [ -e "$OP_ROOT/$rel" ]; then
      mkdir -p "$backup_root/$(dirname "$rel")" || return $?
      cp -a "$OP_ROOT/$rel" "$backup_root/$rel" || return $?
    fi
  done < <(managed_targets)
  printf '%s\n' "$backup_root" || return $?
}

restore_managed_targets_from_backup() {
  local backup_root="$1"
  local rel=""
  local restore_ec=0
  [ -d "$backup_root" ] || return 1
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if [ -e "$backup_root/$rel" ]; then
      if ! mkdir -p "$OP_ROOT/$(dirname "$rel")"; then
        echo "WARN: failed to create parent for managed revert rollback target: $rel" >&2
        restore_ec=1
        continue
      fi
      if ! cp -a "$backup_root/$rel" "$OP_ROOT/$rel"; then
        echo "WARN: failed to restore managed revert rollback target from backup: $rel" >&2
        restore_ec=1
      fi
    else
      if ! rm -f "$OP_ROOT/$rel"; then
        echo "WARN: failed to remove managed revert rollback target absent from backup: $rel" >&2
        restore_ec=1
      fi
    fi
  done < <(managed_targets)
  return "$restore_ec"
}

reset_managed_targets() {
  local rel=""
  local reset_ec=0
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if ! git -C "$OP_ROOT" reset -q HEAD -- "$rel" >/dev/null 2>&1; then
      echo "WARN: failed to reset managed target index: $rel" >&2
      reset_ec=1
    fi
    if git -C "$OP_ROOT" ls-files --error-unmatch -- "$rel" >/dev/null 2>&1; then
      if ! git -C "$OP_ROOT" checkout -- "$rel"; then
        echo "WARN: failed to restore tracked managed target from HEAD: $rel" >&2
        reset_ec=1
      fi
    else
      if ! rm -f "$OP_ROOT/$rel"; then
        echo "WARN: failed to remove untracked managed target: $rel" >&2
        reset_ec=1
      fi
    fi
  done < <(managed_targets)
  return "$reset_ec"
}

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "WARN: upstream repo not found at $OP_ROOT; skipping onroad UI export transformer revert" >&2
  exit 0
fi

if ! backup_root="$(backup_managed_targets)"; then
  echo "ERROR: failed to back up managed onroad UI export transformer targets; refusing revert" >&2
  exit 1
fi
echo "WARN: reverting managed onroad UI export transformer targets" >&2
echo "WARN: backups written to $backup_root" >&2
reset_ec=0
reset_managed_targets || reset_ec=$?
if [ "$reset_ec" -ne 0 ]; then
  restore_ec=0
  restore_managed_targets_from_backup "$backup_root" || restore_ec=$?
  if [ "$restore_ec" -eq 0 ]; then
    echo "ERROR: revert failed; restored managed targets from $backup_root" >&2
    exit "$reset_ec"
  fi
  echo "ERROR: revert failed and rollback failed or incomplete from $backup_root" >&2
  exit "$restore_ec"
fi
rm -f "$STATE_ENV" "$STATE_JSON" "$RESTART_MARKER"
restart_openpilot_ui_if_offroad

echo "CommaView onroad UI export transformer reverted"
