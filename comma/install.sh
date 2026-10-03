#!/usr/bin/env bash
# CommaView installer for comma devices (AGNOS/openpilot/sunnypilot)
# Installs a prebuilt C++ commaviewd bundle from a pinned release, published at
# commaview.com/commaviewd/<tag>/ (another release repository's come from its GitHub releases).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"
VERSION_ENV="${SCRIPT_DIR}/version.env"
GITHUB_REPO="${COMMAVIEWD_RELEASE_REPO:-RhynoTech/commaviewd}"
RELEASES_API_URL="${COMMAVIEWD_RELEASES_API_URL:-https://api.github.com/repos/${GITHUB_REPO}/releases?per_page=20}"
# The runtime paired with the current CommaView app release. It describes RhynoTech/commaviewd
# only, so another release repo skips it unless the URL is set explicitly.
CURRENT_RELEASE_URL="${COMMAVIEWD_CURRENT_RELEASE_URL:-}"
if [ -z "$CURRENT_RELEASE_URL" ] && [ "$GITHUB_REPO" = "RhynoTech/commaviewd" ]; then
  CURRENT_RELEASE_URL="https://commaview.com/api/current-release"
fi
# Where RhynoTech/commaviewd's releases are published: <origin>/<tag>/ holds the tarball, its
# .sha256 and the installer's companions (comma/). Not GitHub, so the repository can be private.
RELEASES_ORIGIN=""
if [ "$GITHUB_REPO" = "RhynoTech/commaviewd" ]; then
  RELEASES_ORIGIN="${COMMAVIEWD_RELEASES_ORIGIN:-https://commaview.com/commaviewd}"
fi

resolve_latest_release_tag() {
  curl -fsSL --retry 3 --retry-delay 1 "$RELEASES_API_URL" \
    | tr -d '\r' \
    | grep -m1 '"tag_name":' \
    | sed -E 's/.*"tag_name":[[:space:]]*"([^"]+)".*/\1/' || true
}

resolve_current_release_tag() {
  [ -n "$CURRENT_RELEASE_URL" ] || return 0
  curl -fsL --max-time 5 --retry 1 --retry-delay 1 "$CURRENT_RELEASE_URL" \
    | tr -d '\r\n' \
    | sed -nE 's/.*"runtimeTag"[[:space:]]*:[[:space:]]*"(v[^"]+)".*/\1/p' || true
}

INSTALLED_RELEASE_TAG=""
if [ -f "$VERSION_ENV" ]; then
  # shellcheck disable=SC1090
  . "$VERSION_ENV"
  INSTALLED_RELEASE_TAG="${RELEASE_TAG:-}"
fi

USE_CURRENT_RELEASE=0
RELEASE_TAG="${COMMAVIEWD_RELEASE_TAG:-}"
VERSION="${COMMAVIEWD_VERSION:-}"
ASSET_NAME=""
ASSET_SHA_NAME=""
BASE_URL=""
INSTALLER_REF=""
INSTALLER_RAW_BASE=""

resolve_release_inputs() {
  if [ -z "$RELEASE_TAG" ]; then
    if [ "$USE_CURRENT_RELEASE" = "1" ] && [ -n "$INSTALLED_RELEASE_TAG" ]; then
      RELEASE_TAG="$INSTALLED_RELEASE_TAG"
    elif [ -n "${COMMAVIEWD_DEFAULT_TAG:-}" ]; then
      RELEASE_TAG="$COMMAVIEWD_DEFAULT_TAG"
    elif [ -n "${COMMAVIEWD_INSTALLER_REF:-}" ] && [[ "${COMMAVIEWD_INSTALLER_REF}" == v* ]]; then
      RELEASE_TAG="$COMMAVIEWD_INSTALLER_REF"
    else
      # Prefer the runtime the current app release uses, so a runtime that's tagged but
      # not yet paired with an app release isn't installed by default.
      RELEASE_TAG="$(resolve_current_release_tag)"
      if [ -z "$RELEASE_TAG" ]; then
        RELEASE_TAG="$(resolve_latest_release_tag)"
      fi
    fi
  fi
  if [ -z "$RELEASE_TAG" ]; then
    RELEASE_TAG="v0.0.1-alpha"
  fi

  VERSION="${COMMAVIEWD_VERSION:-${VERSION:-${RELEASE_TAG#v}}}"
  ASSET_NAME="${COMMAVIEWD_ASSET_NAME:-commaview-comma-${RELEASE_TAG}.tar.gz}"
  ASSET_SHA_NAME="${ASSET_NAME}.sha256"
  # Keep installer companions pinned to the same resolved release by default.
  # Falling back to a branch here mixes release assets with moving companion scripts.
  INSTALLER_REF="${COMMAVIEWD_INSTALLER_REF:-$RELEASE_TAG}"
  if [ -n "$RELEASES_ORIGIN" ]; then
    BASE_URL="${COMMAVIEWD_BASE_URL:-${RELEASES_ORIGIN}/${RELEASE_TAG}}"
  else
    BASE_URL="${COMMAVIEWD_BASE_URL:-https://github.com/${GITHUB_REPO}/releases/download/${RELEASE_TAG}}"
  fi
  # A release's companions are published with it; another ref (a branch, while developing) is
  # read from the repository.
  if [ -n "$RELEASES_ORIGIN" ] && [[ "$INSTALLER_REF" == v* ]]; then
    INSTALLER_RAW_BASE="${COMMAVIEWD_INSTALLER_RAW_BASE:-${RELEASES_ORIGIN}/${INSTALLER_REF}/comma}"
  else
    INSTALLER_RAW_BASE="${COMMAVIEWD_INSTALLER_RAW_BASE:-https://raw.githubusercontent.com/${GITHUB_REPO}/${INSTALLER_REF}/comma}"
  fi
}

# --tag and --current pick the release while the arguments are parsed, so only look up the
# default (which needs the network) when neither is given.
default_release_needed=1
for arg in "$@"; do
  case "$arg" in
    --tag|--current) default_release_needed=0 ;;
  esac
done
if [ "$default_release_needed" = "1" ]; then
  resolve_release_inputs
fi

INSTALL_DIR="/data/commaview"
CONTINUE_SH="/data/continue.sh"
MARKER="# commaview-hook"
# Runs synchronously before continue.sh execs launch_openpilot.sh: start.sh applies
# the onroad UI export patch before openpilot's manager loads its UI, then starts
# the runtime in the background. Earlier installs ran "start.sh &" alongside
# launch_openpilot.sh, which raced manager loading the UI.
BOOT_HOOK_CMD="/data/commaview/start.sh --before-openpilot"
PARAMS_DIR="/data/params/d"
FORCE_OFFROAD=0
# Exit status of an install queued until openpilot is offroad (EX_TEMPFAIL).
DEFERRED_EXIT=75
DEFERRED_DIR="${COMMAVIEWD_DEFERRED_DIR:-/data/commaview-deferred}"
tmpdir=""
COMPANION_DIR=""  # set to "$tmpdir/companions" by refresh_required_files
BACKUP_ROOT="${COMMAVIEWD_BACKUP_ROOT:-/data/commaview-backups}"
INSTALL_ROLLBACK_BACKUP_ROOT="${COMMAVIEWD_INSTALL_ROLLBACK_BACKUP_ROOT:-$BACKUP_ROOT/install-rollback}"
INSTALL_MUTATED=0
INSTALL_SUCCESS=0
PRESERVE_TMPDIR=0
ONROAD_UI_EXPORT_APPLIED=0

usage() {
  cat <<USAGE
CommaView installer ${VERSION}

Usage:
  install.sh [--tag <release-tag>] [--current] [--force-offroad] [--help]

Options:
  --tag <release-tag>            Install or update to a specific release tag.
  --current                      Reinstall the installed release instead of looking one up.
  --force-offroad                While driving, queue the install to run once the car is parked or offroad
                                 (exit 75) instead of refusing it (exit 42). Never asks openpilot to go offroad.
  -h, --help                     Show this help and exit.

Without --tag or --current, installs the runtime paired with the current CommaView app
release, or the newest GitHub release if that lookup fails.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --tag)
      [ "$#" -ge 2 ] || { echo "ERROR: --tag requires a value" >&2; exit 1; }
      RELEASE_TAG="$2"
      VERSION=""
      resolve_release_inputs
      shift 2
      ;;
    --current)
      USE_CURRENT_RELEASE=1
      RELEASE_TAG=""
      VERSION=""
      resolve_release_inputs
      shift
      ;;
    --force-offroad)
      FORCE_OFFROAD=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "ERROR: missing required command: $1" >&2
    exit 1
  }
}

read_param() {
  local path="$PARAMS_DIR/$1"
  [ -f "$path" ] || return 0
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

# Prints 1 while the car is being driven: onroad and not parked. Parked is in Park, at a standstill
# and not engaged (sunnypilot's MADS included), as commaviewd road-phase reads it from openpilot's
# queues without subscribing. A runtime without road-phase, or one that can't tell, leaves onroad
# as driving.
ROAD_PHASE_BIN="${COMMAVIEWD_ROAD_PHASE_BIN:-${INSTALL_DIR:-/data/commaview}/commaviewd}"
read_is_driving() {
  [ "$(read_is_onroad)" = "1" ] || { echo 0; return 0; }
  if [ -x "$ROAD_PHASE_BIN" ] && timeout 5 "$ROAD_PHASE_BIN" road-phase >/dev/null 2>&1; then
    echo 0
  else
    echo 1
  fi
}


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

# Polls every 0.2s, up to <attempts> times, until none of <pids> is alive.
wait_for_pids_exit() {
  local attempts="$1"
  local pids="$2"
  local pid remaining
  for _ in $(seq 1 "$attempts"); do
    remaining=""
    for pid in $pids; do
      if kill -0 "$pid" 2>/dev/null; then
        remaining="$remaining $pid"
      fi
    done
    [ -z "$remaining" ] && return 0
    sleep 0.2
  done
  return 1
}

stop_commaview_processes() {
  local pids
  # start.sh's supervisors restart a crashed process while they still own their pid file.
  rm -f "$INSTALL_DIR/run/bridge-supervisor.pid" "$INSTALL_DIR/run/control-supervisor.pid"
  pids="$(commaview_pids | tr '\n' ' ')"
  [ -n "$pids" ] || return 0

  # shellcheck disable=SC2086
  kill $pids 2>/dev/null || true
  wait_for_pids_exit 25 "$pids" && return 0

  # shellcheck disable=SC2086
  kill -9 $pids 2>/dev/null || true
  wait_for_pids_exit 10 "$pids" && return 0

  return 1
}

ensure_commaview_stopped() {
  if ! stop_commaview_processes; then
    echo "ERROR: failed to stop existing CommaView runtime processes: $(commaview_pids | tr '\n' ' ')" >&2
    exit 1
  fi
  leftover="$(commaview_pids | tr '\n' ' ')"
  if [ -n "$leftover" ]; then
    echo "ERROR: CommaView runtime processes still running after stop: $leftover" >&2
    exit 1
  fi
  rm -f "$INSTALL_DIR/run/bridge.pid" "$INSTALL_DIR/run/control.pid"
}


preserve_install_rollback_backup() {
  local backup_dir="$1"
  local preserved_dir=""
  mkdir -p "$INSTALL_ROLLBACK_BACKUP_ROOT" || return $?
  preserved_dir="$(mktemp -d "$INSTALL_ROLLBACK_BACKUP_ROOT/$(date -u +%Y%m%d-%H%M%S).XXXXXX")" || return $?
  cp -a "$backup_dir"/. "$preserved_dir"/ || return $?
  printf '%s\n' "$preserved_dir" || return $?
}

restore_previous_install_tree() {
  local backup_dir="$tmpdir/previous-install"
  local restore_ec=0
  local preserved_dir=""
  [ "$INSTALL_MUTATED" = "1" ] || return 0
  [ "$INSTALL_SUCCESS" = "0" ] || return 0
  [ -d "$backup_dir" ] || return 0

  echo "WARN: install failed after modifying live tree; restoring previous CommaView files" >&2
  rm -f \
    "$INSTALL_DIR/commaviewd" \
    "$INSTALL_DIR/VERSION" \
    "$INSTALL_DIR/start.sh" \
    "$INSTALL_DIR/stop.sh" \
    "$INSTALL_DIR/uninstall.sh" \
    "$INSTALL_DIR/runtime-debug.defaults.json" \
    "$INSTALL_DIR/version.env"
  rm -rf \
    "$INSTALL_DIR/lib" \
    "$INSTALL_DIR/scripts" \
    "$INSTALL_DIR/patches" \
    "$INSTALL_DIR/src"
  if cp -a "$backup_dir"/. "$INSTALL_DIR"/; then
    :
  else
    restore_ec=$?
    echo "ERROR: failed to restore previous CommaView install tree from $backup_dir" >&2
    if preserved_dir="$(preserve_install_rollback_backup "$backup_dir")"; then
      echo "ERROR: preserved failed install rollback backup at $preserved_dir" >&2
    else
      PRESERVE_TMPDIR=1
      echo "ERROR: failed to preserve rollback backup under $INSTALL_ROLLBACK_BACKUP_ROOT; keeping temporary backup at $backup_dir" >&2
    fi
    return "$restore_ec"
  fi
  if [ -x "$INSTALL_DIR/start.sh" ]; then
    echo "WARN: restarting restored CommaView runtime after failed install" >&2
    COMMAVIEWD_RESTART_REASON=install-rollback bash "$INSTALL_DIR/start.sh" >/dev/null 2>&1 || \
      echo "WARN: failed to restart restored CommaView runtime" >&2
  fi
}

revert_onroad_ui_export_after_failed_install() {
  local revert_helper="$INSTALL_DIR/scripts/revert_onroad_ui_export_patch.sh"
  local revert_args=()
  local revert_ec=0
  [ "$ONROAD_UI_EXPORT_APPLIED" = "1" ] || return 0
  [ "$INSTALL_SUCCESS" = "0" ] || return 0

  if [ ! -x "$revert_helper" ]; then
    echo "ERROR: install failed after applying onroad UI export transformer, but revert helper is missing; preserving $INSTALL_DIR for recovery" >&2
    PRESERVE_TMPDIR=1
    return 1
  fi

  if [ "$FORCE_OFFROAD" = "1" ]; then
    revert_args+=(--force-offroad)
  fi

  echo "WARN: install failed after applying onroad UI export transformer; reverting upstream UI files" >&2
  COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" bash "$revert_helper" "${revert_args[@]}" || revert_ec=$?
  if [ "$revert_ec" -ne 0 ]; then
    echo "ERROR: failed to revert onroad UI export transformer after failed install; preserving $INSTALL_DIR for recovery" >&2
    PRESERVE_TMPDIR=1
    return "$revert_ec"
  fi
  return 0
}

cleanup() {
  local restore_ec=0
  [ -n "${COMMAVIEWD_INSTALLER_HANDOFF_FILE:-}" ] && rm -f "$COMMAVIEWD_INSTALLER_HANDOFF_FILE"
  if revert_onroad_ui_export_after_failed_install; then
    restore_previous_install_tree || restore_ec=$?
  else
    restore_ec=$?
    echo "ERROR: skipping previous install restore because upstream UI transformer revert failed" >&2
  fi
  if [ "$PRESERVE_TMPDIR" = "1" ]; then
    echo "WARN: preserving installer temporary directory for rollback diagnostics: $tmpdir" >&2
  elif [ -n "${tmpdir:-}" ]; then
    rm -rf "$tmpdir"
  fi
  return "$restore_ec"
}


# Queues this install in run_when_offroad.sh, which runs it once the car has been parked (or
# openpilot offroad) for a little while. The queued copy is this release's own installer, pinned to the tag resolved now.
queue_install_until_offroad() {
  local runner="$COMPANION_DIR/scripts/run_when_offroad.sh"
  if [ ! -f "$runner" ]; then
    echo "ERROR: install blocked while driving, and this release cannot queue it. Park the vehicle and retry." >&2
    exit 42
  fi
  if ! COMMAVIEWD_DEFERRED_DIR="$DEFERRED_DIR" bash "$runner" queue install --file "$COMPANION_DIR/install.sh" -- \
      bash @JOB@/install.sh --tag "$RELEASE_TAG" >/dev/null; then
    echo "ERROR: install blocked while driving and could not be queued. Park the vehicle and retry." >&2
    exit 42
  fi
  echo "DEFERRED: CommaView ${RELEASE_TAG} will install once the car is in Park with openpilot disengaged, or offroad."
  echo "COMMAVIEW_MAINTENANCE_DEFERRED=install"
  exit "$DEFERRED_EXIT"
}

# The release being installed installs itself: an installer asked for another release (the comma's
# own update queues the installed install.sh with --tag) hands over to that release's install.sh,
# fetched with its companions, before it changes anything. Piped installs (curl | bash) already run
# the release's own installer.
handoff_to_release_installer() {
  [ "${COMMAVIEWD_INSTALLER_HANDOFF:-0}" = "1" ] && return 0
  local self="${BASH_SOURCE[0]:-}" next="$COMPANION_DIR/install.sh" copy
  [ -f "$self" ] && [ -f "$next" ] || return 0
  [ "$(sha256sum < "$self")" = "$(sha256sum < "$next")" ] && return 0
  copy="$(mktemp /tmp/commaview-install-next.XXXXXX)" || return 0
  cp -f "$next" "$copy" || { rm -f "$copy"; return 0; }
  local args=(--tag "$RELEASE_TAG")
  [ "$FORCE_OFFROAD" = "1" ] && args+=(--force-offroad)
  echo "Handing over to the ${RELEASE_TAG} installer"
  rm -rf "$tmpdir"
  trap - EXIT
  COMMAVIEWD_INSTALLER_HANDOFF=1 COMMAVIEWD_INSTALLER_HANDOFF_FILE="$copy" exec bash "$copy" "${args[@]}"
}

# A queued job would undo or repeat what an install run directly does now.
cancel_deferred_maintenance() {
  [ "${COMMAVIEWD_DEFERRED_JOB:-0}" = "1" ] && return 0
  [ -f "$DEFERRED_DIR/runner.sh" ] || return 0
  COMMAVIEWD_DEFERRED_DIR="$DEFERRED_DIR" bash "$DEFERRED_DIR/runner.sh" cancel >/dev/null 2>&1 || true
}

# Installing restarts the runtime and rewrites openpilot's UI files (which its running UI never
# reloads), so it runs offroad or parked, never while driving. CommaView never asks openpilot to go
# offroad (no OffroadMode): with --force-offroad an install asked for while driving is queued until
# the car is parked instead.
ensure_offroad_ready() {
  if [ "$(read_is_driving)" != "1" ]; then
    cancel_deferred_maintenance
    return 0
  fi

  if [ "$FORCE_OFFROAD" != "1" ]; then
    echo "ERROR: install blocked while driving. Shift into Park (openpilot not engaged), or rerun with --force-offroad to install once it is parked." >&2
    exit 42
  fi
  queue_install_until_offroad
}

copy_required_file() {
  local src_rel="$1"
  local dst="$2"
  local mode="${3:-755}"
  local src="$COMPANION_DIR/$src_rel"

  if [ ! -f "$src" ]; then
    echo "ERROR: missing required installer file: $src_rel" >&2
    exit 1
  fi

  mkdir -p "$(dirname "$dst")"
  cp "$src" "$dst"
  chmod "$mode" "$dst"
}

required_files=(
  "install.sh"
  "start.sh"
  "stop.sh"
  "uninstall.sh"
  "runtime-debug.defaults.json"
  "scripts/verify_onroad_ui_export_patch.sh"
  "scripts/apply_onroad_ui_export_patch.sh"
  "scripts/revert_onroad_ui_export_patch.sh"
  "scripts/smoke_onroad_ui_export_helper.py"
  "scripts/transform_onroad_ui_export.py"
  "scripts/run_when_offroad.sh"
  "src/commaview_export.openpilot.py"
  "src/commaview_export.sunnypilot.py"
  "src/commaview_drive_stats.py"
)

refresh_required_files() {
  local rel dst url dir
  local failed=()

  COMPANION_DIR="$tmpdir/companions"
  mkdir -p "$COMPANION_DIR"

  for rel in "${required_files[@]}"; do
    dst="$COMPANION_DIR/$rel"
    url="${INSTALLER_RAW_BASE}/$rel"
    dir="$(dirname "$dst")"

    if ! mkdir -p "$dir"; then
      echo "ERROR: unable to create directory for installer companion files: $dir" >&2
      failed+=("$rel")
      continue
    fi

    echo "Fetching installer companion: $rel"
    if ! curl -fsSL --retry 3 --retry-delay 1 -o "$dst" "$url"; then
      rm -f "$dst"
      failed+=("$rel")
    fi
  done

  if [ "${#failed[@]}" -gt 0 ]; then
    echo "ERROR: failed to fetch required installer files from ${INSTALLER_RAW_BASE}:" >&2
    for rel in "${failed[@]}"; do
      echo "  - $rel" >&2
    done
    exit 1
  fi
}

validate_required_files() {
  local missing=()
  local rel
  for rel in "${required_files[@]}"; do
    if [ ! -f "$COMPANION_DIR/$rel" ]; then
      missing+=("$COMPANION_DIR/$rel")
    fi
  done

  if [ "${#missing[@]}" -gt 0 ]; then
    echo "ERROR: missing required installer files:" >&2
    for path in "${missing[@]}"; do
      echo "  - $path" >&2
    done
    exit 1
  fi
}

deploy_required_scripts() {
  mkdir -p "$INSTALL_DIR/src"
  copy_required_file "install.sh" "$INSTALL_DIR/install.sh"
  copy_required_file "start.sh" "$INSTALL_DIR/start.sh"
  copy_required_file "runtime-debug.defaults.json" "$INSTALL_DIR/runtime-debug.defaults.json" 644
  copy_required_file "scripts/verify_onroad_ui_export_patch.sh" "$INSTALL_DIR/scripts/verify_onroad_ui_export_patch.sh"
  copy_required_file "scripts/apply_onroad_ui_export_patch.sh" "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh"
  copy_required_file "scripts/revert_onroad_ui_export_patch.sh" "$INSTALL_DIR/scripts/revert_onroad_ui_export_patch.sh"
  copy_required_file "scripts/smoke_onroad_ui_export_helper.py" "$INSTALL_DIR/scripts/smoke_onroad_ui_export_helper.py"
  copy_required_file "scripts/transform_onroad_ui_export.py" "$INSTALL_DIR/scripts/transform_onroad_ui_export.py"
  copy_required_file "scripts/run_when_offroad.sh" "$INSTALL_DIR/scripts/run_when_offroad.sh"
  # Remove the retired experimental subprocess helper when upgrading from that build.
  rm -f "$INSTALL_DIR/scripts/commaview_export_worker.py"
  copy_required_file "src/commaview_export.openpilot.py" "$INSTALL_DIR/src/commaview_export.openpilot.py" 644
  copy_required_file "src/commaview_export.sunnypilot.py" "$INSTALL_DIR/src/commaview_export.sunnypilot.py" 644
  copy_required_file "src/commaview_drive_stats.py" "$INSTALL_DIR/src/commaview_drive_stats.py" 644
  copy_required_file "stop.sh" "$INSTALL_DIR/stop.sh"
  copy_required_file "uninstall.sh" "$INSTALL_DIR/uninstall.sh"

  cat > "$INSTALL_DIR/version.env" <<EOF
VERSION="${VERSION}"
RELEASE_TAG="${RELEASE_TAG}"
EOF
  chmod 644 "$INSTALL_DIR/version.env"
}


ensure_api_auth_token() {
  local token_path="/data/commaview/api/auth.token"
  if [ -s "$token_path" ]; then
    chmod 600 "$token_path" 2>/dev/null || true
    return 0
  fi

  echo "Generating CommaView API auth token..."
  umask 077
  python3 - <<'PYTOKEN' > "$token_path"
import secrets
print(secrets.token_urlsafe(32))
PYTOKEN
  chmod 600 "$token_path" 2>/dev/null || true
}

# The new runtime is healthy once its control API answers: what the app (and the next update)
# talks to.
wait_for_control_api() {
  local _
  for _ in $(seq 1 "${COMMAVIEWD_INSTALL_HEALTH_TIMEOUT_SEC:-30}"); do
    curl -fsS --max-time 2 http://127.0.0.1:5002/commaview/version >/dev/null 2>&1 && return 0
    sleep 1
  done
  return 1
}

print_pairing_code() {
  local token_path="/data/commaview/api/auth.token"
  local token=""
  local response=""
  local pair_code=""

  if [ -r "$token_path" ]; then
    token="$(tr -d '\r\n' < "$token_path")"
  fi

  if [ -z "$token" ]; then
    echo "Pair code: unavailable (missing API token)"
    return 0
  fi

  response="$(curl -fsS --retry 5 --retry-delay 1 -X POST \
    -H "X-CommaView-Token: $token" \
    http://127.0.0.1:5002/pairing/create 2>/dev/null || true)"
  pair_code="$(PAIRING_RESPONSE="$response" python3 - <<'PYPAIR'
import json
import os
try:
    data = json.loads(os.environ.get("PAIRING_RESPONSE", ""))
    print(data.get("pairCode", ""))
except Exception:
    print("")
PYPAIR
)"

  if [ -n "$pair_code" ]; then
    echo ""
    echo "CommaView pair code: $pair_code"
    echo "Enter this one-time pair code in the CommaView app."
  else
    echo "Pair code: unavailable (open CommaView settings to generate one after install)"
  fi
}

# Installs or upgrades the continue.sh boot hook. The hook goes right before
# continue.sh's "exec ... launch_openpilot" line; without one there is nowhere
# safe to put it, so an existing hook is left as it is.
install_boot_hook() {
  [ -f "$CONTINUE_SH" ] || return 0
  if ! grep -q '^exec .*launch_openpilot' "$CONTINUE_SH"; then
    echo "WARN: $CONTINUE_SH has no exec launch_openpilot line; boot hook not changed" >&2
    return 0
  fi
  if grep -qF "$MARKER" "$CONTINUE_SH" && grep -qxF "$BOOT_HOOK_CMD" "$CONTINUE_SH" && \
     [ "$(grep -c 'commaview/start.sh' "$CONTINUE_SH")" = "1" ]; then
    echo "Boot hook already present"
    return 0
  fi
  if grep -qF "$MARKER" "$CONTINUE_SH" || grep -q 'commaview/start.sh' "$CONTINUE_SH"; then
    sed -i '/# commaview-hook/d; /commaview\/start.sh/d' "$CONTINUE_SH"
    echo "Upgrading boot hook to prepare the onroad UI export before openpilot starts"
  fi
  sed -i "/^exec .*launch_openpilot/i\\
$MARKER\\
$BOOT_HOOK_CMD" "$CONTINUE_SH"
  echo "Boot hook installed"
}

backup_managed_install_tree() {
  local backup_dir="$tmpdir/previous-install"
  mkdir -p "$backup_dir"
  for rel in \
    commaviewd \
    VERSION \
    start.sh \
    stop.sh \
    uninstall.sh \
    runtime-debug.defaults.json \
    version.env \
    lib \
    scripts \
    patches \
    vendor \
    src; do
    [ -e "$INSTALL_DIR/$rel" ] || continue
    cp -a "$INSTALL_DIR/$rel" "$backup_dir/$rel"
  done
}

clean_managed_install_tree() {
  echo "Removing stale managed CommaView files..."
  INSTALL_MUTATED=1
  rm -f \
    "$INSTALL_DIR/commaviewd" \
    "$INSTALL_DIR/VERSION" \
    "$INSTALL_DIR/start.sh" \
    "$INSTALL_DIR/stop.sh" \
    "$INSTALL_DIR/uninstall.sh" \
    "$INSTALL_DIR/runtime-debug.defaults.json" \
    "$INSTALL_DIR/version.env"
  rm -rf \
    "$INSTALL_DIR/lib" \
    "$INSTALL_DIR/scripts" \
    "$INSTALL_DIR/patches" \
    "$INSTALL_DIR/vendor" \
    "$INSTALL_DIR/src" \
    "$INSTALL_DIR/run"
  mkdir -p \
    "$INSTALL_DIR/logs" \
    "$INSTALL_DIR/run" \
    "$INSTALL_DIR/lib" \
    "$INSTALL_DIR/api" \
    "$INSTALL_DIR/config"
  rm -f \
    "$INSTALL_DIR/config/hud-lite-patch.env"
}

need_cmd curl
need_cmd tar
need_cmd sha256sum
need_cmd cp

tmpdir="$(mktemp -d /tmp/commaview-install.XXXXXX)"
trap cleanup EXIT
refresh_required_files
validate_required_files
handoff_to_release_installer

echo "=== CommaView ${VERSION} Installer ==="
echo "Release: ${RELEASE_TAG}"
echo "Repo:    ${GITHUB_REPO}"

ensure_offroad_ready

mkdir -p "$INSTALL_DIR/logs" "$INSTALL_DIR/run" "$INSTALL_DIR/lib" "$INSTALL_DIR/api"

echo "Downloading release assets..."
curl -fL --retry 3 --retry-delay 1 -o "$tmpdir/$ASSET_NAME" "$BASE_URL/$ASSET_NAME"
curl -fL --retry 3 --retry-delay 1 -o "$tmpdir/$ASSET_SHA_NAME" "$BASE_URL/$ASSET_SHA_NAME"

expected_sha="$(awk 'NF{print $1; exit}' "$tmpdir/$ASSET_SHA_NAME" | tr -d '\r\n')"
if ! echo "$expected_sha" | grep -Eq '^[0-9a-fA-F]{64}$'; then
  echo "ERROR: invalid sha256 file format: $ASSET_SHA_NAME" >&2
  exit 1
fi
actual_sha="$(sha256sum "$tmpdir/$ASSET_NAME" | awk '{print $1}')"
if [ "$expected_sha" != "$actual_sha" ]; then
  echo "ERROR: checksum mismatch" >&2
  echo "  expected: $expected_sha" >&2
  echo "  actual:   $actual_sha" >&2
  exit 1
fi

STAGED_BUNDLE="$tmpdir/staged-bundle"
mkdir -p "$STAGED_BUNDLE"
echo "Staging and validating bundle..."
tar -xzf "$tmpdir/$ASSET_NAME" -C "$STAGED_BUNDLE" --strip-components=1
if [ ! -f "$STAGED_BUNDLE/commaviewd" ]; then
  echo "ERROR: bundle missing commaviewd" >&2
  exit 1
fi
staged_capnp_lib_count=$(find "$STAGED_BUNDLE/lib" -maxdepth 1 -type f -name 'libcapnp-*.so' 2>/dev/null | wc -l | tr -d ' ')
staged_kj_lib_count=$(find "$STAGED_BUNDLE/lib" -maxdepth 1 -type f -name 'libkj-*.so' 2>/dev/null | wc -l | tr -d ' ')
if [ "$staged_capnp_lib_count" -eq 0 ] || [ "$staged_kj_lib_count" -eq 0 ]; then
  echo "ERROR: bundle missing required runtime libs" >&2
  exit 1
fi

backup_managed_install_tree

echo "Stopping existing CommaView processes..."
ensure_commaview_stopped

clean_managed_install_tree

echo "Installing staged bundle..."
cp -a "$STAGED_BUNDLE"/. "$INSTALL_DIR"/

deploy_required_scripts
ensure_api_auth_token
echo "Applying direct v2 onroad UI export patch lifecycle..."
if [ -x "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh" ]; then
  COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" bash "$INSTALL_DIR/scripts/apply_onroad_ui_export_patch.sh" --force-repair
  ONROAD_UI_EXPORT_APPLIED=1
else
  echo "ERROR: missing onroad UI export patch apply helper" >&2
  exit 1
fi

if [ ! -f "$INSTALL_DIR/commaviewd" ]; then
  echo "ERROR: bundle missing $INSTALL_DIR/commaviewd" >&2
  exit 1
fi
capnp_lib_count=$(find "$INSTALL_DIR/lib" -maxdepth 1 -type f -name 'libcapnp-*.so' | wc -l | tr -d ' ')
kj_lib_count=$(find "$INSTALL_DIR/lib" -maxdepth 1 -type f -name 'libkj-*.so' | wc -l | tr -d ' ')
if [ "$capnp_lib_count" -eq 0 ] || [ "$kj_lib_count" -eq 0 ]; then
  echo "ERROR: bundle missing required runtime libs in $INSTALL_DIR/lib" >&2
  exit 1
fi

chmod +x "$INSTALL_DIR/commaviewd"
BINARY_SIZE=$(ls -lh "$INSTALL_DIR/commaviewd" | awk '{print $5}')

install_boot_hook

echo "Starting CommaView runtime..."
bash "$INSTALL_DIR/start.sh"
if ! wait_for_control_api; then
  # The cleanup trap puts the previous release back and starts it.
  echo "ERROR: the ${RELEASE_TAG} runtime did not answer on its control API; rolling back" >&2
  stop_commaview_processes || true
  exit 1
fi
print_pairing_code
INSTALL_SUCCESS=1

echo ""
echo "=== CommaView ${VERSION} installed ==="
if [ -f "$INSTALL_DIR/run/onroad-ui-export-ui-restart-needed" ]; then
  echo "  Reboot the comma device to load the onroad UI export: openpilot cannot reload"
  echo "  its running UI, so CommaView leaves it alone until openpilot next starts."
fi
echo "  Source:      ${BASE_URL}/${ASSET_NAME}"
echo "  Binary:      $INSTALL_DIR/commaviewd ($BINARY_SIZE)"
echo "  Runtime:     commaviewd dual-mode (bridge + control)"
echo "  Direct v2 onroad UI export: install-time patch lifecycle enforced"
echo "  Install/update: bash $INSTALL_DIR/install.sh [--tag <release-tag>] [--current] [--force-offroad]"
echo "  Uninstall:   bash $INSTALL_DIR/uninstall.sh"
