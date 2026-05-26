#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
SRC_ROOT="$INSTALL_DIR/src"
TRANSFORMER="$INSTALL_DIR/scripts/transform_onroad_ui_export.py"
VERIFY_SCRIPT="$INSTALL_DIR/scripts/verify_onroad_ui_export_patch.sh"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"
RESTART_MARKER="$INSTALL_DIR/run/onroad-ui-export-ui-restart-needed"
PARAMS_DIR="/data/params/d"
FORCE_OFFROAD=0
FORCE_OFFROAD_OWNED=0
FORCE_OFFROAD_PREV=""
FORCE_REPAIR=0

read_param() {
  local path="$PARAMS_DIR/$1"
  [[ -f "$path" ]] || return 0
  tr -d '\000\r\n' < "$path" 2>/dev/null || true
}

write_param() {
  mkdir -p "$PARAMS_DIR"
  printf '%s' "$2" > "$PARAMS_DIR/$1"
}

request_openpilot_ui_restart() {
  mkdir -p "$(dirname "$RESTART_MARKER")"
  printf 'pending\n' > "$RESTART_MARKER"
}

restore_force_offroad_mode() {
  if [ "$FORCE_OFFROAD_OWNED" = "1" ]; then
    write_param "OffroadMode" "${FORCE_OFFROAD_PREV:-0}"
  fi
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

  echo "INFO: restarting openpilot UI to load CommaView onroad UI export transformer output" >&2
  pkill -INT -f "selfdrive.ui.ui" 2>/dev/null || true
  sleep 2
  rm -f "$RESTART_MARKER"
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
    echo "ERROR: socket UI export transformer apply blocked while onroad" >&2
    exit 42
  fi

  FORCE_OFFROAD_PREV="$(read_param OffroadMode)"
  if [ "$FORCE_OFFROAD_PREV" != "1" ]; then
    echo "INFO: requesting OffroadMode for transformer apply" >&2
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
    --force-repair) FORCE_REPAIR=1; shift ;;
    -h|--help) echo "Usage: apply_onroad_ui_export_patch.sh [--force-offroad] [--force-repair]"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

trap cleanup EXIT
ensure_offroad_ready

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "ERROR: upstream repo not found at $OP_ROOT" >&2
  exit 1
fi

managed_targets() {
  printf '%s\n' \
    "selfdrive/ui/commaview_export.py" \
    "selfdrive/ui/ui_state.py" \
    "selfdrive/ui/mici/onroad/augmented_road_view.py" \
    "selfdrive/ui/onroad/augmented_road_view.py"
}

backup_managed_targets() {
  local backup_parent="$INSTALL_DIR/backups/onroad-ui-export"
  local backup_root=""
  local rel=""
  mkdir -p "$backup_parent"
  backup_root="$(mktemp -d "$backup_parent/$(date -u +%Y%m%d-%H%M%S).XXXXXX")"
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if [ -e "$OP_ROOT/$rel" ]; then
      mkdir -p "$backup_root/$(dirname "$rel")"
      cp -a "$OP_ROOT/$rel" "$backup_root/$rel"
    fi
  done < <(managed_targets)
  printf '%s\n' "$backup_root"
}

restore_managed_targets_from_backup() {
  local backup_root="$1"
  [ -d "$backup_root" ] || return 0
  cp -a "$backup_root"/. "$OP_ROOT"/ 2>/dev/null || true
}

dirty_managed_targets() {
  local rel=""
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    git -C "$OP_ROOT" status --porcelain -- "$rel"
  done < <(managed_targets)
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

force_repair_managed_targets() {
  local backup_root=""
  backup_root="$(backup_managed_targets)"
  echo "WARN: force repairing managed onroad UI export transformer targets" >&2
  echo "WARN: backups written to $backup_root" >&2
  reset_managed_targets
}

state_value() {
  local key="$1"
  [ -f "$STATE_ENV" ] || return 1
  sed -n "s/^${key}=//p" "$STATE_ENV" | tail -n 1 | sed 's/^"//; s/"$//'
}

remote_flavor() {
  local remote="$1"
  remote="${remote%.git}"
  remote="${remote%/}"
  case "$remote" in
    *github.com:commaai/openpilot|*github.com/commaai/openpilot) printf '%s\n' openpilot ;;
    *github.com:sunnypilot/sunnypilot|*github.com/sunnypilot/sunnypilot|*github.com:sunnypilot/openpilot|*github.com/sunnypilot/openpilot) printf '%s\n' sunnypilot ;;
    *) return 1 ;;
  esac
}

detect_flavor() {
  local preferred=""
  local remote=""
  local state_flavor=""
  local state_op_root=""

  remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
  if [ -n "$remote" ]; then
    preferred="$(remote_flavor "$remote")" || return 1
  else
    state_flavor="$(state_value ONROAD_UI_EXPORT_FLAVOR || true)"
    state_op_root="$(state_value ONROAD_UI_EXPORT_OP_ROOT || true)"
    if [ "$state_op_root" = "$OP_ROOT" ] && { [ "$state_flavor" = "openpilot" ] || [ "$state_flavor" = "sunnypilot" ]; }; then
      preferred="$state_flavor"
    fi
  fi

  if [ -n "$preferred" ] && [ -f "$SRC_ROOT/commaview_export.${preferred}.py" ]; then
    printf '%s\n' "$preferred"
    return 0
  fi
  return 1
}

flavor="$(detect_flavor)" || {
  echo "ERROR: unsupported upstream remote for $OP_ROOT; CommaView currently supports only commaai/openpilot and sunnypilot remotes" >&2
  exit 1
}
template="$SRC_ROOT/commaview_export.${flavor}.py"
[ -f "$template" ] || { echo "ERROR: missing socket UI export helper template: $template" >&2; exit 1; }
[ -f "$TRANSFORMER" ] || { echo "ERROR: missing socket UI export transformer: $TRANSFORMER" >&2; exit 1; }

if [ "$FORCE_REPAIR" != "1" ] && [ -x "$VERIFY_SCRIPT" ] && "$VERIFY_SCRIPT" --json >/dev/null 2>&1; then
  request_openpilot_ui_restart
  restart_openpilot_ui_if_offroad
  exit 0
fi

if dirty_targets="$(dirty_managed_targets)" && [ -n "$dirty_targets" ]; then
  if [ "$FORCE_REPAIR" != "1" ]; then
    echo "ERROR: onroad UI export transformer target files have local changes:" >&2
    printf '%s\n' "$dirty_targets" >&2
    echo "ERROR: refusing to modify dirty upstream files without --force-repair" >&2
    exit 44
  fi
  force_repair_managed_targets
elif [ "$FORCE_REPAIR" = "1" ]; then
  force_repair_managed_targets
fi

transform_backup_root="$(backup_managed_targets)"
if python3 "$TRANSFORMER" --op-root "$OP_ROOT" --flavor "$flavor"; then
  :
else
  transform_ec=$?
  reset_managed_targets
  restore_managed_targets_from_backup "$transform_backup_root"
  echo "ERROR: transformer failed; restored managed targets from $transform_backup_root" >&2
  exit "$transform_ec"
fi

fingerprint="$(sha256sum "$TRANSFORMER" "$template" | sha256sum | awk '{print $1}')"
mkdir -p "$(dirname "$STATE_ENV")"
printf 'ONROAD_UI_EXPORT_FLAVOR=%s\nONROAD_UI_EXPORT_METHOD=transformer\nONROAD_UI_EXPORT_TRANSFORMER_SHA=%s\nONROAD_UI_EXPORT_OP_ROOT=%s\n' "$flavor" "$fingerprint" "$OP_ROOT" > "$STATE_ENV"

if [ -x "$VERIFY_SCRIPT" ]; then
  request_openpilot_ui_restart
  restart_openpilot_ui_if_offroad
  exec "$VERIFY_SCRIPT" --json
fi

request_openpilot_ui_restart
restart_openpilot_ui_if_offroad
