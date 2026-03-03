#!/usr/bin/env bash
# CommaView installer for comma 4 (AGNOS/sunnypilot)
# Installs prebuilt C++ bridge bundle from pinned GitHub release.
set -euo pipefail

VERSION="0.1.1-alpha"
RELEASE_TAG="v0.1.1-alpha"
GITHUB_REPO="${COMMAVIEW_RELEASE_REPO:-RhynoTech/CommaView}"
ASSET_NAME="${COMMAVIEW_ASSET_NAME:-commaview-comma4-${RELEASE_TAG}.tar.gz}"
ASSET_SHA_NAME="${ASSET_NAME}.sha256"
BASE_URL="${COMMAVIEW_BASE_URL:-https://github.com/${GITHUB_REPO}/releases/download/${RELEASE_TAG}}"

INSTALL_DIR="/data/commaview"
CONTINUE_SH="/data/continue.sh"
MARKER="# commaview-hook"

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

tmpdir="$(mktemp -d /tmp/commaview-install.XXXXXX)"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

mkdir -p "$INSTALL_DIR/logs" "$INSTALL_DIR/run" "$INSTALL_DIR/lib"

echo "Stopping existing CommaView processes..."
pkill -f "hevc-bridge" 2>/dev/null || true
pkill -f "commaview/bridge.py" 2>/dev/null || true
pkill -f "commaview-supervisor.sh" 2>/dev/null || true
pkill -f "/data/commaview/commaview-bridge" 2>/dev/null || true
pkill -f "system/loggerd/encoderd --stream" 2>/dev/null || true
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

cat > "$INSTALL_DIR/start.sh" <<'STARTEOF'
#!/usr/bin/env bash
set +e
LOG=/data/commaview/logs
RUN=/data/commaview/run
mkdir -p "$LOG" "$RUN"

bash /data/commaview/stop.sh >/dev/null 2>&1 || true
nohup /data/commaview/commaview-supervisor.sh >> "$LOG/supervisor.log" 2>&1 &
echo $! > "$RUN/supervisor.pid"
echo "CommaView supervisor started"
STARTEOF

cat > "$INSTALL_DIR/stop.sh" <<'STOPEOF'
#!/usr/bin/env bash
set +e
RUN=/data/commaview/run

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

for f in bridge.pid encoderd_stream.pid camerad_offroad.pid; do
  if [ -f "$RUN/$f" ]; then
    pid=$(cat "$RUN/$f" 2>/dev/null)
    kill "$pid" 2>/dev/null || true
    sleep 0.2
    kill -9 "$pid" 2>/dev/null || true
    rm -f "$RUN/$f"
  fi
done

pkill -f 'commaview-supervisor.sh' 2>/dev/null || true
pkill -f '/data/commaview/commaview-bridge' 2>/dev/null || true
pkill -f 'system/loggerd/encoderd --stream' 2>/dev/null || true

echo "CommaView stopped"
STOPEOF

cat > "$INSTALL_DIR/uninstall.sh" <<'UNINSTALLEOF'
#!/usr/bin/env bash
set +e
echo "Stopping services..."
bash /data/commaview/stop.sh 2>/dev/null || true
echo "Removing boot hook..."
sed -i '/# commaview-hook/d; /commaview\/start.sh/d' /data/continue.sh 2>/dev/null || true
echo "Removing files..."
rm -rf /data/commaview
echo "CommaView uninstalled"
UNINSTALLEOF

chmod +x \
  "$INSTALL_DIR/commaview-bridge" \
  "$INSTALL_DIR/commaview-supervisor.sh" \
  "$INSTALL_DIR/start.sh" \
  "$INSTALL_DIR/stop.sh" \
  "$INSTALL_DIR/uninstall.sh"

rm -f "$INSTALL_DIR/hevc-bridge.py" "$INSTALL_DIR/bridge.py" "$INSTALL_DIR/api.py" 2>/dev/null || true

if [ -f "$CONTINUE_SH" ] && ! grep -q "$MARKER" "$CONTINUE_SH"; then
  sed -i "/^exec .*launch_openpilot/i\\
$MARKER\\
/data/commaview/start.sh &" "$CONTINUE_SH"
  echo "Boot hook installed"
elif grep -q "$MARKER" "$CONTINUE_SH" 2>/dev/null; then
  echo "Boot hook already present"
fi

echo ""
echo "=== CommaView ${VERSION} installed ==="
echo "  Source:      ${BASE_URL}/${ASSET_NAME}"
echo "  Binary:      $INSTALL_DIR/commaview-bridge ($BINARY_SIZE)"
echo "  Supervisor:  mode-aware onroad/offroad startup"
echo "  Onroad:      manager encoderd + bridge(prod)"
echo "  Offroad:     camerad + encoderd --stream + bridge(--dev)"
echo "  Uninstall:   bash /data/commaview/uninstall.sh"
