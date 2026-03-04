#!/usr/bin/env bash
# CommaView installer for comma 4 (AGNOS/sunnypilot)
# Production-minimal runtime: commaview-bridge only (no extra watchdog process).
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
echo "WARNING: Use at your own risk. Do not use while driving in violation of local law."
echo "Release: ${RELEASE_TAG}"
echo "Repo:    ${GITHUB_REPO}"

tmpdir="$(mktemp -d /tmp/commaview-install.XXXXXX)"
cleanup() { rm -rf "$tmpdir"; }
trap cleanup EXIT

mkdir -p "$INSTALL_DIR/logs" "$INSTALL_DIR/lib"

echo "Stopping existing CommaView process..."
if [ -x "$INSTALL_DIR/stop.sh" ]; then
  bash "$INSTALL_DIR/stop.sh" 2>/dev/null || true
fi
pkill -f '/data/commaview/commaview-bridge' 2>/dev/null || true
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

if [ ! -x "$INSTALL_DIR/commaview-bridge" ]; then
  echo "ERROR: bundle missing executable $INSTALL_DIR/commaview-bridge" >&2
  exit 1
fi
if [ ! -f "$INSTALL_DIR/lib/libcapnp-0.8.0.so" ] || [ ! -f "$INSTALL_DIR/lib/libkj-0.8.0.so" ]; then
  echo "ERROR: bundle missing required runtime libs in $INSTALL_DIR/lib" >&2
  exit 1
fi

chmod +x "$INSTALL_DIR/commaview-bridge"
BINARY_SIZE=$(ls -lh "$INSTALL_DIR/commaview-bridge" | awk '{print $5}')

cat > "$INSTALL_DIR/start.sh" <<'STARTEOF'
#!/usr/bin/env bash
set +e
LOG_DIR=/data/commaview/logs
PID_FILE=/data/commaview/bridge.pid
mkdir -p "$LOG_DIR"

if [ -f "$PID_FILE" ]; then
  pid=$(cat "$PID_FILE" 2>/dev/null)
  if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    echo "CommaView bridge already running (pid=$pid)"
    exit 0
  fi
  rm -f "$PID_FILE"
fi

cd /data/openpilot
nohup nice -n 19 /data/commaview/commaview-bridge >> "$LOG_DIR/bridge.log" 2>&1 &
echo $! > "$PID_FILE"
echo "CommaView bridge started"
STARTEOF

cat > "$INSTALL_DIR/stop.sh" <<'STOPEOF'
#!/usr/bin/env bash
set +e
PID_FILE=/data/commaview/bridge.pid

if [ -f "$PID_FILE" ]; then
  pid=$(cat "$PID_FILE" 2>/dev/null)
  if [ -n "$pid" ]; then
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 20); do
      kill -0 "$pid" 2>/dev/null || break
      sleep 0.2
    done
    kill -9 "$pid" 2>/dev/null || true
  fi
  rm -f "$PID_FILE"
else
  pkill -f '/data/commaview/commaview-bridge' 2>/dev/null || true
fi

echo "CommaView bridge stopped"
STOPEOF

cat > "$INSTALL_DIR/uninstall.sh" <<'UNINSTALLEOF'
#!/usr/bin/env bash
set +e
echo "Stopping CommaView..."
bash /data/commaview/stop.sh 2>/dev/null || true

echo "Removing boot hook..."
sed -i '/# commaview-hook/d; /commaview\/start.sh/d' /data/continue.sh 2>/dev/null || true

echo "Removing CommaView files..."
rm -rf /data/commaview

echo "CommaView uninstalled"
UNINSTALLEOF

chmod +x \
  "$INSTALL_DIR/commaview-bridge" \
  "$INSTALL_DIR/start.sh" \
  "$INSTALL_DIR/stop.sh" \
  "$INSTALL_DIR/uninstall.sh"

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
echo "  Runtime:     bridge only (no supervisor/watchdog)"
echo "  Priority:    nice +19 (lowest priority, openpilot wins CPU)"
echo "  Uninstall:   bash /data/commaview/uninstall.sh"

echo ""
echo "Starting CommaView now..."
bash "$INSTALL_DIR/start.sh"
