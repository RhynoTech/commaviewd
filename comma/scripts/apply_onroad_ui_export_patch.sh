#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${COMMAVIEWD_INSTALL_DIR:-/data/commaview}"
OP_ROOT="${COMMAVIEWD_OP_ROOT:-/data/openpilot}"
SRC_ROOT="$INSTALL_DIR/src"
TRANSFORMER="$INSTALL_DIR/scripts/transform_onroad_ui_export.py"
VERIFY_SCRIPT="$INSTALL_DIR/scripts/verify_onroad_ui_export_patch.sh"
STATE_ENV="$INSTALL_DIR/config/onroad-ui-export-patch.env"
RESTART_MARKER="$INSTALL_DIR/run/onroad-ui-export-ui-restart-needed"
PARAMS_DIR="${COMMAVIEWD_PARAMS_DIR:-/data/params/d}"
PROC_ROOT="${COMMAVIEWD_PROC_ROOT:-/proc}"
STAGING_ROOT="${COMMAVIEWD_STAGING_ROOT:-/data/safe_staging}"
FORCE_OFFROAD=0
FORCE_OFFROAD_OWNED=0
FORCE_OFFROAD_PREV=""
FORCE_REPAIR=0
BEFORE_OPENPILOT=0
UI_PREFIX=""
REQUESTED_UI_PLATFORM="auto"

# Read-only git commands must not rewrite .git/index: launch_chffrplus.sh skips a
# staged openpilot update when anything under .git is newer than .overlay_init.
export GIT_OPTIONAL_LOCKS=0

read_param() {
  local path="$PARAMS_DIR/$1"
  [[ -f "$path" ]] || return 0
  tr -d '\000\r\n' < "$path" 2>/dev/null || true
}

# openpilot and sunnypilot publish IsOffroad; upstream no longer has an IsOnroad
# param, so reading only IsOnroad always looked offroad - even while driving.
# IsOnroad stays a fallback for older builds and bench rigs. Prints 1 when onroad.
read_is_onroad() {
  case "$(read_param IsOffroad)" in
    0) echo 1 ;;
    1) echo 0 ;;
    *) if [ "$(read_param IsOnroad)" = "1" ]; then echo 1; else echo 0; fi ;;
  esac
}

write_param() {
  mkdir -p "$PARAMS_DIR"
  printf '%s' "$2" > "$PARAMS_DIR/$1"
}

# openpilot cannot reload its UI in place, so CommaView never signals it:
# - openpilot's manager never restarts a process that exits. PythonProcess.start()
#   returns early while self.proc is set, and only stop() clears it, which manager
#   calls for "ui" (always_run) only when it shuts down. The UI exits 0 on SIGINT
#   (gui_app's handler calls sys.exit(0)), so a signalled UI stays dead until reboot.
# - sunnypilot release branches restart a dead "ui" (restart_if_crash=True), but
#   the manager preimports every process module at startup (PythonProcess.prepare)
#   and forks the new UI from it, so the restarted UI runs the code it had before.
# Patched UI files therefore take effect only when the manager starts: at boot,
# where start.sh --before-openpilot applies the patch before launch_openpilot.sh.
# A patch applied while openpilot runs records when it was written so
# verify_onroad_ui_export_patch.sh can report "uiReloadPending" (reboot needed)
# until openpilot next starts.

# Sets OPENPILOT_PROCESS_KIND to "ui" or "manager" when <proc dir> is openpilot's
# UI (setproctitle names it "openpilot.selfdrive.ui.ui" or "selfdrive.ui.ui") or its
# manager ("python3 ./manager.py"); returns 1 otherwise. Sets a variable instead of
# printing so scanning every process costs no forks.
openpilot_process_kind() {
  local first cmd
  local -a args=()
  OPENPILOT_PROCESS_KIND=""
  { mapfile -t -d '' args < "$1/cmdline"; } 2>/dev/null || return 1
  [ "${#args[@]}" -gt 0 ] || return 1
  first="${args[0]%"${args[0]##*[![:space:]]}"}"
  case "$first" in
    *selfdrive.ui.ui) OPENPILOT_PROCESS_KIND="ui"; return 0 ;;
  esac
  cmd=" ${args[*]} "
  case "$cmd" in
    *" ./manager.py "*|*"/system/manager/manager.py "*|*"system.manager.manager "*|*"/selfdrive/manager/manager.py "*|*"selfdrive.manager.manager "*)
      OPENPILOT_PROCESS_KIND="manager"
      return 0
      ;;
  esac
  return 1
}

openpilot_process_pids() {
  local proc
  for proc in "$PROC_ROOT"/[0-9]*; do
    openpilot_process_kind "$proc" && printf '%s\n' "${proc##*/}"
  done
  return 0
}

openpilot_running() {
  [ -n "$(openpilot_process_pids | head -n 1)" ]
}

current_boot_id() {
  tr -d '\r\n' < "$PROC_ROOT/sys/kernel/random/boot_id" 2>/dev/null || true
}

# Centiseconds since boot, the unit verify compares against openpilot's start time.
current_uptime_cs() {
  local up sec frac
  up="$(cut -d' ' -f1 < "$PROC_ROOT/uptime" 2>/dev/null || true)"
  case "$up" in
    ''|*[!0-9.]*) return 1 ;;
  esac
  sec="${up%%.*}"
  frac="${up#*.}"
  [ "$frac" = "$up" ] && frac=""
  frac="${frac}00"
  printf '%s\n' "$((10#${sec:-0} * 100 + 10#${frac:0:2}))"
}

note_openpilot_ui_reload_needed() {
  if [ "${COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART:-0}" = "1" ]; then
    echo "INFO: skipping openpilot UI reload bookkeeping by request" >&2
    return 0
  fi

  if ! openpilot_running; then
    rm -f "$RESTART_MARKER"
    echo "INFO: openpilot is not running; its UI loads the CommaView onroad UI export when it starts" >&2
    return 0
  fi

  mkdir -p "$(dirname "$RESTART_MARKER")"
  printf 'pending\nbootId=%s\npatchedAtUptimeCs=%s\n' "$(current_boot_id)" "$(current_uptime_cs || true)" > "$RESTART_MARKER"
  echo "INFO: CommaView onroad UI export takes effect after the next reboot; openpilot cannot reload its running UI, so CommaView leaves it alone" >&2
}

# launch_chffrplus.sh installs a finalized openpilot update (moves it into place)
# before it starts manager. Mirror its checks so --before-openpilot patches the
# tree that is about to run rather than the one about to be replaced.
openpilot_root_after_launch() {
  local dir="$1"
  if [ ! -f "$dir/.overlay_init" ] || \
     [ -n "$(find "$dir/.git" -newer "$dir/.overlay_init" -print -quit 2>/dev/null)" ] || \
     [ ! -f "$STAGING_ROOT/finalized/.overlay_consistent" ] || \
     [ -d "$STAGING_ROOT/old_openpilot" ]; then
    printf '%s\n' "$dir"
    return 0
  fi
  printf '%s\n' "$STAGING_ROOT/finalized"
}

# Called with --before-openpilot when openpilot has not started: nothing has
# loaded the UI yet, so whatever is on disk now is what the UI will run.
clear_openpilot_ui_reload_if_not_running() {
  [ "$BEFORE_OPENPILOT" = "1" ] || return 0
  [ "${COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART:-0}" = "1" ] && return 0
  openpilot_running && return 0
  rm -f "$RESTART_MARKER"
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
    is_onroad="$(read_is_onroad)"
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
  if [ "$BEFORE_OPENPILOT" = "1" ]; then
    if ! openpilot_running; then
      # Before manager starts nothing drives, and IsOffroad still holds the last
      # session's value (manager clears it at startup), so it is not consulted.
      echo "INFO: openpilot has not started; applying onroad UI export before its UI loads" >&2
      return 0
    fi
    echo "WARN: --before-openpilot given but openpilot is already running; applying the onroad check" >&2
  fi
  is_onroad="$(read_is_onroad)"
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
    --before-openpilot) BEFORE_OPENPILOT=1; shift ;;
    --platform) REQUESTED_UI_PLATFORM="${2:-}"; shift 2 ;;
    --platform=*) REQUESTED_UI_PLATFORM="${1#--platform=}"; shift ;;
    -h|--help) echo "Usage: apply_onroad_ui_export_patch.sh [--force-offroad] [--force-repair] [--before-openpilot] [--platform auto|mici|tizi|tici]"; exit 0 ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 1 ;;
  esac
done

trap cleanup EXIT
ensure_offroad_ready

if [ "$BEFORE_OPENPILOT" = "1" ] && ! openpilot_running; then
  launch_root="$(openpilot_root_after_launch "$OP_ROOT")"
  if [ "$launch_root" != "$OP_ROOT" ]; then
    echo "INFO: launch_openpilot.sh is about to install the staged openpilot update at $launch_root; patching it instead of $OP_ROOT" >&2
    OP_ROOT="$launch_root"
  fi
fi
# verify_onroad_ui_export_patch.sh reads the tree from the environment.
export COMMAVIEWD_OP_ROOT="$OP_ROOT"

if ! git -C "$OP_ROOT" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "ERROR: upstream repo not found at $OP_ROOT" >&2
  exit 1
fi


detect_ui_prefix() {
  if [ -f "$OP_ROOT/selfdrive/ui/ui_state.py" ]; then
    UI_PREFIX=""
  elif [ -f "$OP_ROOT/openpilot/selfdrive/ui/ui_state.py" ]; then
    UI_PREFIX="openpilot/"
  else
    UI_PREFIX=""
  fi
}

normalize_ui_platform() {
  case "$1" in
    auto) printf '%s\n' auto ;;
    mici) printf '%s\n' mici ;;
    tizi|tici) printf '%s\n' tizi ;;
    *) return 1 ;;
  esac
}

read_device_model() {
  local model_path="/sys/firmware/devicetree/base/model"
  [ -f "$model_path" ] || return 1
  tr -d '\000\r\n' < "$model_path" | sed 's/^comma //'
}

detect_ui_platform() {
  local requested="$1"
  local state_platform=""
  local state_op_root=""
  local device_model=""
  if [ "$requested" != "auto" ]; then
    normalize_ui_platform "$requested"
    return $?
  fi
  device_model="$(read_device_model || true)"
  case "$device_model" in
    *mici*|*MICI*|*comma\ 4*) printf '%s\n' mici; return 0 ;;
    *tizi*|*TIZI*|*tici*|*TICI*|*comma\ 3*) printf '%s\n' tizi; return 0 ;;
  esac
  state_platform="$(state_value ONROAD_UI_EXPORT_UI_PLATFORM || true)"
  state_op_root="$(state_value ONROAD_UI_EXPORT_OP_ROOT || true)"
  if [ "$state_op_root" = "$OP_ROOT" ] && normalize_ui_platform "$state_platform" >/dev/null 2>&1; then
    normalize_ui_platform "$state_platform"
    return 0
  fi
  if [ -f "$OP_ROOT/${UI_PREFIX}selfdrive/ui/mici/onroad/augmented_road_view.py" ] && [ ! -f "$OP_ROOT/${UI_PREFIX}selfdrive/ui/onroad/augmented_road_view.py" ]; then
    printf '%s\n' mici
    return 0
  fi
  if [ -f "$OP_ROOT/${UI_PREFIX}selfdrive/ui/onroad/augmented_road_view.py" ] && [ ! -f "$OP_ROOT/${UI_PREFIX}selfdrive/ui/mici/onroad/augmented_road_view.py" ]; then
    printf '%s\n' tizi
    return 0
  fi
  printf '%s\n' auto
}

detect_ui_prefix

managed_targets() {
  printf '%s\n' \
    "${UI_PREFIX}selfdrive/ui/commaview_export.py" \
    "${UI_PREFIX}selfdrive/ui/ui_state.py" \
    "${UI_PREFIX}selfdrive/ui/mici/onroad/augmented_road_view.py" \
    "${UI_PREFIX}selfdrive/ui/onroad/augmented_road_view.py"
}

backup_managed_targets() {
  local backup_parent="$INSTALL_DIR/backups/onroad-ui-export"
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
        echo "WARN: failed to create parent for managed rollback target: $rel" >&2
        restore_ec=1
        continue
      fi
      if ! cp -a "$backup_root/$rel" "$OP_ROOT/$rel"; then
        echo "WARN: failed to restore managed rollback target from backup: $rel" >&2
        restore_ec=1
      fi
    else
      if ! rm -f "$OP_ROOT/$rel"; then
        echo "WARN: failed to remove managed rollback target absent from backup: $rel" >&2
        restore_ec=1
      fi
    fi
  done < <(managed_targets)
  return "$restore_ec"
}

rollback_managed_targets_clean() {
  local rollback_dirty=""
  if ! rollback_dirty="$(dirty_managed_targets)"; then
    echo "WARN: failed to verify managed rollback cleanliness" >&2
    return 1
  fi
  if [ -n "$rollback_dirty" ]; then
    echo "WARN: managed rollback incomplete; dirty targets remain:" >&2
    printf '%s\n' "$rollback_dirty" >&2
    return 1
  fi
  return 0
}

managed_targets_match_backup() {
  local backup_root="$1"
  local rel=""
  local match_ec=0
  [ -d "$backup_root" ] || return 1
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if [ -e "$backup_root/$rel" ]; then
      if [ ! -e "$OP_ROOT/$rel" ] || ! cmp -s "$backup_root/$rel" "$OP_ROOT/$rel"; then
        echo "WARN: managed rollback does not match backup for target: $rel" >&2
        match_ec=1
      fi
    else
      if [ -e "$OP_ROOT/$rel" ]; then
        echo "WARN: managed rollback target should be absent but exists: $rel" >&2
        match_ec=1
      fi
    fi
  done < <(managed_targets)
  return "$match_ec"
}

dirty_managed_targets() {
  local rel=""
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    git -C "$OP_ROOT" status --porcelain -- "$rel"
  done < <(managed_targets)
}

# One line per managed target: its sha256, or "absent". Compared before and after
# a run to tell whether the UI files openpilot loaded have changed.
managed_targets_digest() {
  local rel=""
  while IFS= read -r rel; do
    [ -n "$rel" ] || continue
    if [ -f "$OP_ROOT/$rel" ]; then
      printf '%s %s\n' "$(sha256sum < "$OP_ROOT/$rel" | awk '{print $1}')" "$rel"
    else
      printf 'absent %s\n' "$rel"
    fi
  done < <(managed_targets)
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

force_repair_managed_targets() {
  local backup_root=""
  if ! backup_root="$(backup_managed_targets)"; then
    echo "ERROR: failed to back up managed onroad UI export transformer targets; refusing force repair" >&2
    return 1
  fi
  echo "WARN: force repairing managed onroad UI export transformer targets" >&2
  echo "WARN: backups written to $backup_root" >&2
  if ! reset_managed_targets; then
    local restore_ec=0
    local rollback_match_ec=0
    restore_managed_targets_from_backup "$backup_root" || restore_ec=$?
    managed_targets_match_backup "$backup_root" || rollback_match_ec=$?
    if [ "$restore_ec" -eq 0 ] && [ "$rollback_match_ec" -eq 0 ]; then
      echo "ERROR: failed to reset managed onroad UI export transformer targets; restored managed targets from $backup_root" >&2
    else
      echo "ERROR: failed to reset managed onroad UI export transformer targets and rollback failed or incomplete from $backup_root" >&2
    fi
    return 1
  fi
}

state_value() {
  local key="$1"
  [ -f "$STATE_ENV" ] || return 1
  sed -n "s/^${key}=//p" "$STATE_ENV" | tail -n 1 | sed 's/^"//; s/"$//'
}

upstream_head() {
  git -C "$OP_ROOT" rev-parse HEAD 2>/dev/null || true
}

upstream_remote() {
  git -C "$OP_ROOT" remote get-url origin 2>/dev/null || true
}

# Records patch provenance; verify_onroad_ui_export_patch.sh writes the same format.
write_patch_state_env() {
  fingerprint="$(sha256sum "$TRANSFORMER" "$template" | sha256sum | awk '{print $1}')"
  current_upstream_head="$(upstream_head)"
  current_upstream_remote="$(upstream_remote)"
  mkdir -p "$(dirname "$STATE_ENV")"
  printf 'ONROAD_UI_EXPORT_FLAVOR=%s\nONROAD_UI_EXPORT_METHOD=transformer\nONROAD_UI_EXPORT_UI_PLATFORM=%s\nONROAD_UI_EXPORT_TRANSFORMER_SHA=%s\nONROAD_UI_EXPORT_OP_ROOT=%s\nONROAD_UI_EXPORT_UPSTREAM_HEAD=%s\nONROAD_UI_EXPORT_UPSTREAM_REMOTE=%s\n' \
    "$flavor" "$ui_platform" "$fingerprint" "$OP_ROOT" "$current_upstream_head" "$current_upstream_remote" > "$STATE_ENV"
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
ui_platform="$(detect_ui_platform "$REQUESTED_UI_PLATFORM")" || {
  echo "ERROR: unable to determine UI platform for $OP_ROOT; pass --platform mici or --platform tizi" >&2
  exit 1
}
template="$SRC_ROOT/commaview_export.${flavor}.py"
[ -f "$template" ] || { echo "ERROR: missing socket UI export helper template: $template" >&2; exit 1; }
[ -f "$TRANSFORMER" ] || { echo "ERROR: missing socket UI export transformer: $TRANSFORMER" >&2; exit 1; }

clear_openpilot_ui_reload_if_not_running

if [ "$FORCE_REPAIR" != "1" ] && [ -x "$VERIFY_SCRIPT" ] && "$VERIFY_SCRIPT" --json --platform "$ui_platform" >/dev/null 2>&1; then
  # Already patched: no UI file changes, so nothing new for openpilot to reload.
  exit 0
fi

targets_before="$(managed_targets_digest)"

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

if ! transform_backup_root="$(backup_managed_targets)"; then
  echo "ERROR: failed to back up managed onroad UI export transformer targets before transform" >&2
  exit 1
fi
state_env_backup="$transform_backup_root/onroad-ui-export-patch.env.before"
state_env_existed=0
if [ -f "$STATE_ENV" ]; then
  cp "$STATE_ENV" "$state_env_backup"
  state_env_existed=1
fi
if python3 "$TRANSFORMER" --op-root "$OP_ROOT" --flavor "$flavor" --platform "$ui_platform"; then
  :
else
  transform_ec=$?
  reset_ec=0
  reset_managed_targets || reset_ec=$?
  if [ "$reset_ec" -ne 0 ]; then
    echo "WARN: reset of managed targets had errors before rollback restore" >&2
  fi
  restore_ec=0
  rollback_clean_ec=0
  restore_managed_targets_from_backup "$transform_backup_root" || restore_ec=$?
  rollback_managed_targets_clean || rollback_clean_ec=$?
  if [ "$restore_ec" -eq 0 ] && [ "$rollback_clean_ec" -eq 0 ]; then
    echo "ERROR: transformer failed; restored managed targets from $transform_backup_root" >&2
  else
    echo "ERROR: transformer failed and rollback failed or incomplete from $transform_backup_root" >&2
  fi
  exit "$transform_ec"
fi

write_patch_state_env

# Record the reload need before verifying so the status JSON verify prints says
# whether openpilot's running UI still has the old code.
reload_marker_backup="$transform_backup_root/onroad-ui-export-ui-restart-needed.before"
reload_marker_existed=0
if [ -f "$RESTART_MARKER" ]; then
  cp "$RESTART_MARKER" "$reload_marker_backup"
  reload_marker_existed=1
fi
if [ "$(managed_targets_digest)" != "$targets_before" ]; then
  note_openpilot_ui_reload_needed
fi

if [ -x "$VERIFY_SCRIPT" ]; then
  verify_ec=0
  "$VERIFY_SCRIPT" --json --platform "$ui_platform" || verify_ec=$?
  if [ "$verify_ec" -eq 0 ]; then
    exit 0
  fi
  reset_ec=0
  reset_managed_targets || reset_ec=$?
  if [ "$reset_ec" -ne 0 ]; then
    echo "WARN: reset of managed targets had errors before verification rollback restore" >&2
  fi
  restore_ec=0
  rollback_match_ec=0
  restore_managed_targets_from_backup "$transform_backup_root" || restore_ec=$?
  managed_targets_match_backup "$transform_backup_root" || rollback_match_ec=$?
  state_restore_ec=0
  if [ "$state_env_existed" -eq 1 ]; then
    cp "$state_env_backup" "$STATE_ENV" || state_restore_ec=$?
  else
    rm -f "$STATE_ENV" || state_restore_ec=$?
  fi
  if [ "$reload_marker_existed" -eq 1 ]; then
    cp "$reload_marker_backup" "$RESTART_MARKER" || state_restore_ec=$?
  else
    rm -f "$RESTART_MARKER" || state_restore_ec=$?
  fi
  if [ "$state_restore_ec" -ne 0 ]; then
    echo "WARN: failed to restore onroad UI export patch state after verification rollback" >&2
  fi
  if [ "$restore_ec" -eq 0 ] && [ "$rollback_match_ec" -eq 0 ]; then
    echo "ERROR: verification failed; restored managed targets from $transform_backup_root" >&2
  else
    echo "ERROR: verification failed and rollback failed or incomplete from $transform_backup_root" >&2
  fi
  exit "$verify_ec"
fi

write_patch_state_env
