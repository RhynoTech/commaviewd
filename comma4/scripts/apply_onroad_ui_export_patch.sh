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
    "selfdrive/ui/mici/onroad/augmented_road_view.py"
}

backup_managed_targets() {
  local backup_root="$INSTALL_DIR/backups/onroad-ui-export/$(date -u +%Y%m%d-%H%M%S)"
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

detect_flavor() {
  local preferred=""
  local remote=""

  if [ -f "$STATE_ENV" ]; then
    # shellcheck disable=SC1090
    . "$STATE_ENV" || true
    if [ "${ONROAD_UI_EXPORT_OP_ROOT:-}" = "$OP_ROOT" ] && [ -n "${ONROAD_UI_EXPORT_FLAVOR:-}" ] && [ -f "$SRC_ROOT/commaview_export.${ONROAD_UI_EXPORT_FLAVOR}.py" ]; then
      printf '%s\n' "$ONROAD_UI_EXPORT_FLAVOR"
      return 0
    fi
  fi

  remote="$(git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true)"
  if printf '%s' "$remote" | grep -qi 'sunnypilot'; then
    preferred='sunnypilot'
  elif printf '%s' "$remote" | grep -qi 'openpilot'; then
    preferred='openpilot'
  elif grep -Fq 'COMMAVIEW_RUNTIME_FLAVOR = "SUNNYPILOT"' "$OP_ROOT/selfdrive/ui/commaview_export.py" 2>/dev/null; then
    preferred='sunnypilot'
  elif grep -Fq 'COMMAVIEW_RUNTIME_FLAVOR = "OPENPILOT"' "$OP_ROOT/selfdrive/ui/commaview_export.py" 2>/dev/null; then
    preferred='openpilot'
  fi

  if [ -n "$preferred" ] && [ -f "$SRC_ROOT/commaview_export.${preferred}.py" ]; then
    printf '%s\n' "$preferred"
    return 0
  fi
  return 1
}

flavor="$(detect_flavor)" || {
  echo "ERROR: unable to determine supported socket UI export transformer flavor for $OP_ROOT" >&2
  exit 1
}
template="$SRC_ROOT/commaview_export.${flavor}.py"
[ -f "$template" ] || { echo "ERROR: missing socket UI export helper template: $template" >&2; exit 1; }
[ -x "$TRANSFORMER" ] || { echo "ERROR: missing socket UI export transformer: $TRANSFORMER" >&2; exit 1; }

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

python3 "$TRANSFORMER" --op-root "$OP_ROOT" --flavor "$flavor"

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
