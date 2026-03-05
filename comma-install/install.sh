#!/usr/bin/env bash
# CommaView installer for comma 4 (AGNOS/sunnypilot)
# Installs prebuilt C++ bridge bundle from pinned GitHub release.
set -euo pipefail

VERSION="0.1.2-alpha"
RELEASE_TAG="v0.1.2-alpha"
GITHUB_REPO="${COMMAVIEW_RELEASE_REPO:-RhynoTech/CommaView}"
ASSET_NAME="${COMMAVIEW_ASSET_NAME:-commaview-comma4-${RELEASE_TAG}.tar.gz}"
ASSET_SHA_NAME="${ASSET_NAME}.sha256"
BASE_URL="${COMMAVIEW_BASE_URL:-https://github.com/${GITHUB_REPO}/releases/download/${RELEASE_TAG}}"

INSTALL_DIR="/data/commaview"
CONTINUE_SH="/data/continue.sh"
MARKER="# commaview-hook"

ENABLE_TAILSCALE=0
TAILSCALE_AUTHKEY="${COMMAVIEW_TAILSCALE_AUTHKEY:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" 2>/dev/null && pwd || true)"

usage() {
  cat <<USAGE
CommaView installer ${VERSION}

Usage:
  install.sh [--enable-tailscale] [--tailscale-auth-key <key>] [--help]

Options:
  --enable-tailscale             Enable optional Tailscale Guardian setup.
  --tailscale-auth-key <key>     One-time auth key used during initial join.
                                 You can also set COMMAVIEW_TAILSCALE_AUTHKEY.
  -h, --help                     Show this help and exit.

Default behavior:
  If --enable-tailscale is not set, install path is unchanged.
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --enable-tailscale)
      ENABLE_TAILSCALE=1
      shift
      ;;
    --tailscale-auth-key)
      if [ "$#" -lt 2 ]; then
        echo "ERROR: --tailscale-auth-key requires a value" >&2
        exit 1
      fi
      TAILSCALE_AUTHKEY="$2"
      shift 2
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

need_cmd curl
need_cmd tar
need_cmd sha256sum

echo "=== CommaView ${VERSION} Installer ==="
echo "Release: ${RELEASE_TAG}"
echo "Repo:    ${GITHUB_REPO}"
if [ "$ENABLE_TAILSCALE" = "1" ]; then
  echo "Tailscale opt-in: enabled"
fi

tmpdir="$(mktemp -d /tmp/commaview-install.XXXXXX)"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

mkdir -p "$INSTALL_DIR/logs" "$INSTALL_DIR/run" "$INSTALL_DIR/lib" "$INSTALL_DIR/tailscale"

copy_or_embed_tailscale_script() {
  local name="$1"
  local src="$SCRIPT_DIR/tailscale/$name"
  local dst="$INSTALL_DIR/tailscale/$name"

  if [ -f "$src" ]; then
    cp "$src" "$dst"
    chmod +x "$dst"
    return 0
  fi

  case "$name" in
    install_tailscale_runtime.sh)
      cat > "$dst" <<'RUNTIMEEOF'
#!/usr/bin/env bash
set -euo pipefail
TAILSCALE_ROOT="${COMMAVIEW_TAILSCALE_INSTALL_ROOT:-/data/commaview/tailscale}"
BIN_DIR="$TAILSCALE_ROOT/bin"
mkdir -p "$BIN_DIR"
if [ -x "$BIN_DIR/tailscale" ] && [ -x "$BIN_DIR/tailscaled" ]; then
  exit 0
fi
if command -v tailscale >/dev/null 2>&1 && command -v tailscaled >/dev/null 2>&1; then
  cp "$(command -v tailscale)" "$BIN_DIR/tailscale"
  cp "$(command -v tailscaled)" "$BIN_DIR/tailscaled"
  chmod +x "$BIN_DIR/tailscale" "$BIN_DIR/tailscaled"
  exit 0
fi
echo "tailscale runtime helper fallback could not find system tailscale binaries" >&2
exit 1
RUNTIMEEOF
      ;;
    tailscale-guardian.sh)
      cat > "$dst" <<'GUARDIANEOF'
#!/usr/bin/env bash
set -euo pipefail
PARAMS_DIR="${COMMAVIEW_PARAMS_DIR:-/data/params/d}"
RUN_DIR="${COMMAVIEW_RUN_DIR:-/data/commaview/run}"
LOG_DIR="${COMMAVIEW_LOG_DIR:-/data/commaview/logs}"
TAILSCALE_DIR="${COMMAVIEW_TAILSCALE_DIR:-/data/commaview/tailscale}"
TAILSCALE_BIN="${COMMAVIEW_TAILSCALE_BIN:-$TAILSCALE_DIR/bin/tailscale}"
TAILSCALED_BIN="${COMMAVIEW_TAILSCALED_BIN:-$TAILSCALE_DIR/bin/tailscaled}"
SOCKET_PATH="${COMMAVIEW_TAILSCALE_SOCKET:-$TAILSCALE_DIR/state/tailscaled.sock}"
STATE_FILE="${COMMAVIEW_TAILSCALE_STATE_FILE:-$TAILSCALE_DIR/state/tailscaled.state}"
mkdir -p "$RUN_DIR" "$LOG_DIR" "$(dirname "$SOCKET_PATH")"
while true; do
  enabled="$(tr -d '\000\r\n' < "$PARAMS_DIR/CommaViewTailscaleEnabled" 2>/dev/null || true)"
  onroad="$(tr -d '\000\r\n' < "$PARAMS_DIR/IsOnroad" 2>/dev/null || true)"
  if [ "$enabled" = "1" ] && [ "$onroad" != "1" ]; then
    pgrep -f "$TAILSCALED_BIN" >/dev/null 2>&1 || nohup "$TAILSCALED_BIN" --state="$STATE_FILE" --socket="$SOCKET_PATH" >> "$LOG_DIR/tailscale-guardian.log" 2>&1 &
    "$TAILSCALE_BIN" --socket "$SOCKET_PATH" up --accept-routes >> "$LOG_DIR/tailscale-guardian.log" 2>&1 || true
  else
    "$TAILSCALE_BIN" --socket "$SOCKET_PATH" down >> "$LOG_DIR/tailscale-guardian.log" 2>&1 || true
    pkill -f "$TAILSCALED_BIN" 2>/dev/null || true
  fi
  sleep 2
done
GUARDIANEOF
      ;;
    tailscalectl.sh)
      cat > "$dst" <<'CTLLEOF'
#!/usr/bin/env bash
set -euo pipefail
PARAMS_DIR="${COMMAVIEW_PARAMS_DIR:-/data/params/d}"
mkdir -p "$PARAMS_DIR"
cmd="${1:-status}"
case "$cmd" in
  enable) printf "1" > "$PARAMS_DIR/CommaViewTailscaleEnabled" ;;
  disable) printf "0" > "$PARAMS_DIR/CommaViewTailscaleEnabled" ;;
  status) ;;
  *) echo "Usage: tailscalectl.sh <status|enable|disable>"; exit 1 ;;
esac
enabled="$(tr -d '\000\r\n' < "$PARAMS_DIR/CommaViewTailscaleEnabled" 2>/dev/null || echo 0)"
onroad="$(tr -d '\000\r\n' < "$PARAMS_DIR/IsOnroad" 2>/dev/null || echo 0)"
printf '{"enabled":%s,"onroad":%s}\n' "$( [ "$enabled" = "1" ] && echo true || echo false )" "$( [ "$onroad" = "1" ] && echo true || echo false )"
CTLLEOF
      ;;
    *)
      echo "ERROR: unknown tailscale asset $name" >&2
      exit 1
      ;;
  esac

  chmod +x "$dst"
}

deploy_tailscale_assets() {
  copy_or_embed_tailscale_script install_tailscale_runtime.sh
  copy_or_embed_tailscale_script tailscale-guardian.sh
  copy_or_embed_tailscale_script tailscalectl.sh
}


copy_or_embed_upgrade_script() {
  local src="$SCRIPT_DIR/upgrade.sh"
  local dst="$INSTALL_DIR/upgrade.sh"

  if [ -f "$src" ]; then
    cp "$src" "$dst"
    chmod +x "$dst"
    return 0
  fi

  cat > "$dst" <<'UPGRADEEOF'
#!/usr/bin/env bash
set -euo pipefail
DEFAULT_TAG="v0.1.2-alpha"
TAG="${1:-$DEFAULT_TAG}"
curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma-install/upgrade.sh | bash -s -- --tag "$TAG"
UPGRADEEOF
  chmod +x "$dst"
}

prompt_retry_or_continue() {
  if [ ! -t 0 ]; then
    echo "Non-interactive install; continuing without Tailscale." >&2
    return 1
  fi

  while true; do
    echo "Tailscale setup failed. Choose: [r]etry or [c]ontinue without Tailscale"
    read -r choice
    case "${choice:-}" in
      r|R|retry|Retry) return 0 ;;
      c|C|continue|Continue) return 1 ;;
      *) echo "Please enter r or c." ;;
    esac
  done
}

maybe_configure_tailscale_opt_in() {
  [ "$ENABLE_TAILSCALE" = "1" ] || return 0

  local runtime="$INSTALL_DIR/tailscale/install_tailscale_runtime.sh"
  local tailscale_bin="$INSTALL_DIR/tailscale/bin/tailscale"
  local tailscaled_bin="$INSTALL_DIR/tailscale/bin/tailscaled"
  local socket="$INSTALL_DIR/tailscale/state/tailscaled.sock"
  local statefile="$INSTALL_DIR/tailscale/state/tailscaled.state"

  mkdir -p "$INSTALL_DIR/tailscale/state" /data/params/d

  while true; do
    if ! "$runtime"; then
      echo "WARN: failed to install/check tailscale runtime" >&2
      if prompt_retry_or_continue; then
        continue
      fi
      printf "0" > /data/params/d/CommaViewTailscaleEnabled
      return 0
    fi

    if [ -z "${TAILSCALE_AUTHKEY:-}" ]; then
      if [ -t 0 ]; then
        echo "Enter one-time Tailscale auth key (or leave blank to continue without Tailscale):"
        read -r TAILSCALE_AUTHKEY
      fi
    fi

    if [ -z "${TAILSCALE_AUTHKEY:-}" ]; then
      echo "No auth key provided. Continuing without Tailscale setup."
      printf "0" > /data/params/d/CommaViewTailscaleEnabled
      return 0
    fi

    nohup "$tailscaled_bin" --state="$statefile" --socket="$socket" >> "$INSTALL_DIR/logs/tailscale-install.log" 2>&1 &
    local tsd_pid=$!
    sleep 1

    if "$tailscale_bin" --socket "$socket" up --authkey "$TAILSCALE_AUTHKEY" --accept-routes --reset >> "$INSTALL_DIR/logs/tailscale-install.log" 2>&1; then
      "$tailscale_bin" --socket "$socket" down >> "$INSTALL_DIR/logs/tailscale-install.log" 2>&1 || true
      kill "$tsd_pid" 2>/dev/null || true
      wait "$tsd_pid" 2>/dev/null || true
      printf "1" > /data/params/d/CommaViewTailscaleEnabled
      unset TAILSCALE_AUTHKEY
      echo "Tailscale onboarding complete (auth key consumed once)."
      return 0
    fi

    kill "$tsd_pid" 2>/dev/null || true
    wait "$tsd_pid" 2>/dev/null || true

    if prompt_retry_or_continue; then
      continue
    fi

    printf "0" > /data/params/d/CommaViewTailscaleEnabled
    unset TAILSCALE_AUTHKEY
    return 0
  done
}

echo "Stopping existing CommaView processes..."
pkill -f "commaview-supervisor.sh" 2>/dev/null || true
pkill -f "/data/commaview/commaview-bridge" 2>/dev/null || true
pkill -f "system/loggerd/encoderd --stream" 2>/dev/null || true
pkill -f "tailscale-guardian.sh" 2>/dev/null || true
sleep 1

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

echo "Extracting bundle..."
tar -xzf "$tmpdir/$ASSET_NAME" -C "$INSTALL_DIR" --strip-components=1

deploy_tailscale_assets
copy_or_embed_upgrade_script

if [ ! -f "$INSTALL_DIR/commaview-bridge" ]; then
  echo "ERROR: bundle missing $INSTALL_DIR/commaview-bridge" >&2
  exit 1
fi
if [ ! -f "$INSTALL_DIR/lib/libcapnp-0.8.0.so" ] || [ ! -f "$INSTALL_DIR/lib/libkj-0.8.0.so" ]; then
  echo "ERROR: bundle missing required runtime libs in $INSTALL_DIR/lib" >&2
  exit 1
fi

chmod +x "$INSTALL_DIR/commaview-bridge"
BINARY_SIZE=$(ls -lh "$INSTALL_DIR/commaview-bridge" | awk '{print $5}')

# --- supervisor ---
cat > "$INSTALL_DIR/commaview-supervisor.sh" <<'SUPERVISOREOF'
#!/usr/bin/env bash
set -euo pipefail

LOG_DIR=/data/commaview/logs
RUN_DIR=/data/commaview/run
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

ensure_bridge_prod() {
  if is_running_pidfile bridge; then
    local pid cmd
    pid="$(cat "$RUN_DIR/bridge.pid")"
    cmd="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
    if echo "$cmd" | grep -q -- '--dev'; then
      log "bridge running in dev while onroad; restarting prod"
      stop_pidfile bridge
    fi
  fi
  if ! is_running_pidfile bridge; then
    cd /data/openpilot
    log "starting bridge (prod)"
    start_bg bridge nice -n 19 /data/commaview/commaview-bridge
  fi
}

ensure_bridge_dev() {
  if is_running_pidfile bridge; then
    local pid cmd
    pid="$(cat "$RUN_DIR/bridge.pid")"
    cmd="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
    if ! echo "$cmd" | grep -q -- '--dev'; then
      log "bridge running in prod while offroad; restarting dev"
      stop_pidfile bridge
    fi
  fi
  if ! is_running_pidfile bridge; then
    cd /data/openpilot
    log "starting bridge (dev)"
    start_bg bridge nice -n 19 /data/commaview/commaview-bridge --dev
  fi
}

ensure_camerad_offroad() {
  if pgrep -f './camerad' >/dev/null 2>&1; then
    return 0
  fi
  if ! is_running_pidfile camerad_offroad; then
    cd /data/openpilot
    log "starting offroad camerad"
    start_bg camerad_offroad ./system/camerad/camerad
  fi
}

wait_for_camerad_socket() {
  for _ in $(seq 1 30); do
    if pgrep -f './camerad' >/dev/null 2>&1 && [ -S /tmp/visionipc_camerad ]; then
      return 0
    fi
    sleep 1
  done
  return 1
}

ensure_stream_encoderd() {
  if ! is_running_pidfile encoderd_stream; then
    cd /data/openpilot
    log "starting encoderd --stream"
    start_bg encoderd_stream ./system/loggerd/encoderd --stream
  fi
}

stop_stream_encoderd() {
  stop_pidfile encoderd_stream
  pkill -f 'system/loggerd/encoderd --stream' 2>/dev/null || true
}

ensure_offroad_stack() {
  ensure_camerad_offroad
  if ! wait_for_camerad_socket; then
    log "offroad stack waiting: camerad socket not ready"
    return 0
  fi
  ensure_stream_encoderd
  ensure_bridge_dev
}

ensure_onroad_stack() {
  # Let openpilot manager own camerad/encoderd onroad.
  stop_stream_encoderd
  stop_pidfile camerad_offroad
  ensure_bridge_prod
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
  fi

  if [ "$mode" = "onroad" ]; then
    ensure_onroad_stack
  else
    ensure_offroad_stack
  fi

  sleep 2
done
SUPERVISOREOF

# --- start.sh ---
cat > "$INSTALL_DIR/start.sh" <<'STARTEOF'
#!/usr/bin/env bash
set +e
LOG=/data/commaview/logs
RUN=/data/commaview/run
mkdir -p "$LOG" "$RUN"

# Stop stale supervisor and owned children
bash /data/commaview/stop.sh >/dev/null 2>&1 || true

# Launch supervisor
nohup /data/commaview/commaview-supervisor.sh >> "$LOG/supervisor.log" 2>&1 &
echo $! > "$RUN/supervisor.pid"

# Launch tailscale guardian (policy daemon)
if [ -x /data/commaview/tailscale/tailscale-guardian.sh ]; then
  nohup /data/commaview/tailscale/tailscale-guardian.sh >> "$LOG/tailscale-guardian.log" 2>&1 &
  echo $! > "$RUN/tailscale_guardian.pid"
fi

echo "CommaView supervisor started"
STARTEOF

# --- stop.sh ---
cat > "$INSTALL_DIR/stop.sh" <<'STOPEOF'
#!/usr/bin/env bash
set +e
RUN=/data/commaview/run

# stop supervisor first
if [ -f "$RUN/supervisor.pid" ]; then
  pid=$(cat "$RUN/supervisor.pid" 2>/dev/null)
  kill "$pid" 2>/dev/null || true
  for _ in $(seq 1 20); do
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.2
  done
  kill -9 "$pid" 2>/dev/null || true
  rm -f "$RUN/supervisor.pid"
fi

# stop children tracked by pidfiles
for f in bridge.pid encoderd_stream.pid camerad_offroad.pid tailscale_guardian.pid tailscaled.pid; do
  if [ -f "$RUN/$f" ]; then
    pid=$(cat "$RUN/$f" 2>/dev/null)
    kill "$pid" 2>/dev/null || true
    sleep 0.2
    kill -9 "$pid" 2>/dev/null || true
    rm -f "$RUN/$f"
  fi
done

# clean old strays
pkill -f 'commaview-supervisor.sh' 2>/dev/null || true
pkill -f '/data/commaview/commaview-bridge' 2>/dev/null || true
pkill -f 'system/loggerd/encoderd --stream' 2>/dev/null || true
pkill -f 'tailscale-guardian.sh' 2>/dev/null || true
pkill -f '/data/commaview/tailscale/bin/tailscaled' 2>/dev/null || true

if [ -x /data/commaview/tailscale/bin/tailscale ]; then
  /data/commaview/tailscale/bin/tailscale --socket /data/commaview/tailscale/state/tailscaled.sock down >/dev/null 2>&1 || true
fi

echo "CommaView stopped"
STOPEOF

# --- uninstall.sh ---
cat > "$INSTALL_DIR/uninstall.sh" <<'UNINSTALLEOF'
#!/usr/bin/env bash
set +e
echo "Stopping services..."
bash /data/commaview/stop.sh 2>/dev/null || true
echo "Removing boot hook..."
sed -i '/# commaview-hook/d; /commaview\/start.sh/d' /data/continue.sh 2>/dev/null || true
echo "Disabling CommaView tailscale flag..."
echo -n 0 > /data/params/d/CommaViewTailscaleEnabled 2>/dev/null || true
echo "Removing files..."
rm -rf /data/commaview/tailscale
rm -rf /data/commaview
echo "CommaView uninstalled"
UNINSTALLEOF

chmod +x \
  "$INSTALL_DIR/commaview-bridge" \
  "$INSTALL_DIR/commaview-supervisor.sh" \
  "$INSTALL_DIR/start.sh" \
  "$INSTALL_DIR/stop.sh" \
  "$INSTALL_DIR/uninstall.sh" \
  "$INSTALL_DIR/upgrade.sh" \
  "$INSTALL_DIR/tailscale/install_tailscale_runtime.sh" \
  "$INSTALL_DIR/tailscale/tailscale-guardian.sh" \
  "$INSTALL_DIR/tailscale/tailscalectl.sh"

# Hook into continue.sh
if [ -f "$CONTINUE_SH" ] && ! grep -q "$MARKER" "$CONTINUE_SH"; then
  sed -i "/^exec .*launch_openpilot/i\\
$MARKER\\
/data/commaview/start.sh &" "$CONTINUE_SH"
  echo "Boot hook installed"
elif grep -q "$MARKER" "$CONTINUE_SH" 2>/dev/null; then
  echo "Boot hook already present"
fi

maybe_configure_tailscale_opt_in

echo ""
echo "=== CommaView ${VERSION} installed ==="
echo "  Source:      ${BASE_URL}/${ASSET_NAME}"
echo "  Binary:      $INSTALL_DIR/commaview-bridge ($BINARY_SIZE)"
echo "  Supervisor:  mode-aware onroad/offroad startup"
echo "  Onroad:      manager encoderd + bridge(prod)"
echo "  Offroad:     camerad + encoderd --stream + bridge(--dev)"
if [ "$ENABLE_TAILSCALE" = "1" ]; then
  echo "  Tailscale:   opt-in flow complete (see /data/params/d/CommaViewTailscaleEnabled)"
fi
echo "  Upgrade:     bash $INSTALL_DIR/upgrade.sh"
echo "  Uninstall:   bash $INSTALL_DIR/uninstall.sh"
