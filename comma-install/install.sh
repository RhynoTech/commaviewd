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
TMP_DIR="${COMMAVIEW_TAILSCALE_TMP_DIR:-/tmp/commaview-tailscale-runtime}"
TAILSCALE_VERSION="${COMMAVIEW_TAILSCALE_VERSION:-1.80.3}"

log() {
  echo "[tailscale-runtime] $*"
}

detect_arch() {
  local arch
  arch="$(uname -m)"
  case "$arch" in
    aarch64|arm64) echo "arm64" ;;
    x86_64|amd64) echo "amd64" ;;
    *)
      echo "Unsupported architecture: $arch" >&2
      return 1
      ;;
  esac
}

install_from_system_path() {
  local ts_bin tsd_bin
  ts_bin="$(command -v tailscale 2>/dev/null || true)"
  tsd_bin="$(command -v tailscaled 2>/dev/null || true)"

  if [ -n "$ts_bin" ] && [ -n "$tsd_bin" ]; then
    cp "$ts_bin" "$BIN_DIR/tailscale"
    cp "$tsd_bin" "$BIN_DIR/tailscaled"
    chmod +x "$BIN_DIR/tailscale" "$BIN_DIR/tailscaled"
    log "Copied tailscale binaries from system PATH"
    return 0
  fi

  return 1
}

install_from_release_tarball() {
  local arch tarball url extracted_dir
  arch="$(detect_arch)"
  mkdir -p "$TMP_DIR"
  tarball="$TMP_DIR/tailscale_${TAILSCALE_VERSION}_${arch}.tgz"
  url="https://pkgs.tailscale.com/stable/tailscale_${TAILSCALE_VERSION}_${arch}.tgz"

  log "Downloading $url"
  curl -fsSL "$url" -o "$tarball"

  extracted_dir="$TMP_DIR/extract"
  rm -rf "$extracted_dir"
  mkdir -p "$extracted_dir"
  tar -xzf "$tarball" -C "$extracted_dir"

  local ts_bin tsd_bin
  ts_bin="$(find "$extracted_dir" -type f -name tailscale | head -1)"
  tsd_bin="$(find "$extracted_dir" -type f -name tailscaled | head -1)"

  if [ -z "$ts_bin" ] || [ -z "$tsd_bin" ]; then
    echo "Failed to locate tailscale binaries in downloaded archive" >&2
    return 1
  fi

  cp "$ts_bin" "$BIN_DIR/tailscale"
  cp "$tsd_bin" "$BIN_DIR/tailscaled"
  chmod +x "$BIN_DIR/tailscale" "$BIN_DIR/tailscaled"
  log "Installed tailscale binaries from release tarball"
}

main() {
  mkdir -p "$BIN_DIR"

  if [ -x "$BIN_DIR/tailscale" ] && [ -x "$BIN_DIR/tailscaled" ]; then
    log "Runtime already installed at $BIN_DIR"
    exit 0
  fi

  if ! install_from_system_path; then
    install_from_release_tarball
  fi

  if [ ! -x "$BIN_DIR/tailscale" ] || [ ! -x "$BIN_DIR/tailscaled" ]; then
    echo "Failed to install tailscale runtime" >&2
    exit 1
  fi

  log "Runtime ready"
}

main "$@"

RUNTIMEEOF
      ;;
    tailscalectl.sh)
      cat > "$dst" <<'CTLLEOF'
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

copy_or_embed_api_script() {
  local src="$SCRIPT_DIR/commaview-api.py"
  local dst="$INSTALL_DIR/commaview-api.py"

  if [ -f "$src" ]; then
    cp "$src" "$dst"
    chmod +x "$dst"
    return 0
  fi

  cat > "$dst" <<'APIEOF'
#!/usr/bin/env python3
"""CommaView Management API - runs on comma device port 5002"""
import json
import os
import socket
import subprocess
from http.server import HTTPServer, BaseHTTPRequestHandler

VERSION = "0.1.2-alpha"
INSTALL_DIR = "/data/commaview"
BITRATE_FILE = f"{INSTALL_DIR}/stream_bitrate"
DEFAULT_BITRATE = 1000000

TAILSCALECTL = f"{INSTALL_DIR}/tailscale/tailscalectl.sh"


def get_stream_bitrate():
    try:
        with open(BITRATE_FILE) as f:
            return int(f.read().strip())
    except Exception:
        return DEFAULT_BITRATE


def set_stream_bitrate(bitrate):
    with open(BITRATE_FILE, "w") as f:
        f.write(str(bitrate))


def restart_encoderd_stream(bitrate):
    """Kill and restart encoderd --stream with new STREAM_BITRATE."""
    subprocess.run(["pkill", "-f", "encoderd --stream"], capture_output=True)
    import time
    time.sleep(1)
    env = os.environ.copy()
    env["STREAM_BITRATE"] = str(bitrate)
    subprocess.Popen(
        ["/data/openpilot/system/loggerd/encoderd", "--stream"],
        env=env,
        stdout=open(f"{INSTALL_DIR}/logs/encoderd-stream.log", "w"),
        stderr=subprocess.STDOUT,
    )


def tailscale_status():
    if not os.path.exists(TAILSCALECTL):
        return {
            "enabled": False,
            "onroad": False,
            "daemonRunning": False,
            "backendState": "missing",
            "available": False,
        }

    proc = subprocess.run([TAILSCALECTL, "status", "--json"], capture_output=True, text=True)
    if proc.returncode != 0:
        return {
            "enabled": False,
            "onroad": False,
            "daemonRunning": False,
            "backendState": "error",
            "available": True,
            "error": proc.stderr.strip() or proc.stdout.strip() or "tailscalectl status failed",
        }

    try:
        data = json.loads(proc.stdout.strip() or "{}")
    except json.JSONDecodeError:
        data = {
            "enabled": False,
            "onroad": False,
            "daemonRunning": False,
            "backendState": "unknown",
        }

    data["available"] = True
    return data


def tailscale_set_enabled(enable: bool):
    if not os.path.exists(TAILSCALECTL):
        return {"ok": False, "error": "tailscalectl missing", "available": False}

    cmd = "enable" if enable else "disable"
    proc = subprocess.run([TAILSCALECTL, cmd, "--json"], capture_output=True, text=True)
    if proc.returncode != 0:
        return {
            "ok": False,
            "available": True,
            "error": proc.stderr.strip() or proc.stdout.strip() or f"tailscalectl {cmd} failed",
        }

    try:
        payload = json.loads(proc.stdout.strip() or "{}")
    except json.JSONDecodeError:
        payload = {}

    payload.update({"ok": True, "available": True})
    return payload


def tailscale_set_authkey(authkey: str):
    if not os.path.exists(TAILSCALECTL):
        return {"ok": False, "error": "tailscalectl missing", "available": False}
    if not authkey:
        return {"ok": False, "error": "auth key required", "available": True}

    proc = subprocess.run([TAILSCALECTL, "set-auth-key", authkey, "--json"], capture_output=True, text=True)
    if proc.returncode != 0:
        return {
            "ok": False,
            "available": True,
            "error": proc.stderr.strip() or proc.stdout.strip() or "tailscalectl set-auth-key failed",
        }

    try:
        payload = json.loads(proc.stdout.strip() or "{}")
    except json.JSONDecodeError:
        payload = {}

    payload.update({"ok": True, "available": True})
    return payload


MDNS_SERVICE_TYPE = "_commaview._tcp.local."


def is_running(name):
    try:
        r = subprocess.run(["pgrep", "-f", name], capture_output=True)
        return r.returncode == 0
    except Exception:
        return False


def get_ip():
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "unknown"


def get_dongle_id():
    try:
        with open("/data/params/d/DongleId", "r") as f:
            return f.read().strip()
    except Exception:
        return "unknown"


def get_device_type():
    try:
        with open("/sys/firmware/devicetree/base/model", "r") as f:
            return f.read().strip().rstrip("\x00")
    except Exception:
        return "unknown"


def start_mdns():
    """Advertise CommaView via mDNS/Zeroconf so Android app can auto-discover."""
    try:
        from zeroconf import Zeroconf, ServiceInfo
    except ImportError:
        print("[mDNS] zeroconf not installed, skipping mDNS advertisement")
        print("[mDNS] Install with: pip install zeroconf")
        return None, None

    ip = get_ip()
    if ip == "unknown":
        print("[mDNS] No IP address, skipping mDNS")
        return None, None

    dongle_id = get_dongle_id()
    device_type = get_device_type()
    instance_name = f"comma-{dongle_id[-6:] if len(dongle_id) > 6 else dongle_id}"

    info = ServiceInfo(
        MDNS_SERVICE_TYPE,
        f"{instance_name}.{MDNS_SERVICE_TYPE}",
        addresses=[socket.inet_aton(ip)],
        port=5002,
        properties={
            "version": VERSION,
            "dongle_id": dongle_id,
            "device": device_type,
            "webrtc_port": "5001",
            "ws_port": "5003",
            "api_port": "5002",
        },
    )

    zc = Zeroconf()
    zc.register_service(info)
    print(f"[mDNS] Advertising as '{instance_name}' at {ip}")
    return zc, info


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # silence logs

    def _respond(self, code, data):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())


    def _read_json_body(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0

        if length <= 0:
            return {}

        raw = self.rfile.read(length).decode(errors="ignore").strip()
        if not raw:
            return {}

        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {}

    def do_GET(self):
        if self.path == "/commaview/status":
            self._respond(200, {
                "version": VERSION,
                "dongle_id": get_dongle_id(),
                "device": get_device_type(),
                "ip": get_ip(),
                "services": {
                    "camerad": is_running("camerad"),
                    "encoderd_stream": is_running("encoderd --stream"),
                    "webrtcd": is_running("webrtcd.py"),
                    "commaview_api": True,
                },
                "webrtc_port": 5001,
                "api_port": 5002,
                "stream_bitrate": get_stream_bitrate(),
                "tailscale": tailscale_status(),
            })
        elif self.path == "/commaview/version":
            self._respond(200, {"version": VERSION})
        elif self.path == "/commaview/stream/bitrate":
            self._respond(200, {"bitrate": get_stream_bitrate()})
        elif self.path == "/tailscale/status":
            self._respond(200, tailscale_status())
        else:
            self._respond(404, {"error": "not found"})

    def do_POST(self):
        if self.path == "/commaview/camerad/on":
            subprocess.run(["bash", "-c", "echo -n 1 > /data/params/d/IsDriverViewEnabled"])
            self._respond(200, {"ok": True, "camerad": "enabled"})
        elif self.path == "/commaview/camerad/off":
            subprocess.run(["bash", "-c", "echo -n 0 > /data/params/d/IsDriverViewEnabled"])
            self._respond(200, {"ok": True, "camerad": "disabled"})
        elif self.path.startswith("/commaview/stream/bitrate/"):
            try:
                bitrate = int(self.path.split("/")[-1])
                if bitrate < 500000 or bitrate > 10000000:
                    self._respond(400, {"error": "bitrate must be 500000-10000000"})
                    return
                set_stream_bitrate(bitrate)
                restart_encoderd_stream(bitrate)
                self._respond(200, {"ok": True, "bitrate": bitrate})
            except ValueError:
                self._respond(400, {"error": "invalid bitrate"})
        elif self.path == "/commaview/start":
            subprocess.Popen(["bash", f"{INSTALL_DIR}/start.sh"])
            self._respond(200, {"ok": True, "action": "starting"})
        elif self.path == "/commaview/stop":
            subprocess.run(["bash", f"{INSTALL_DIR}/stop.sh"])
            self._respond(200, {"ok": True, "action": "stopped"})
        elif self.path == "/commaview/restart":
            subprocess.run(["bash", f"{INSTALL_DIR}/stop.sh"])
            subprocess.Popen(["bash", f"{INSTALL_DIR}/start.sh"])
            self._respond(200, {"ok": True, "action": "restarting"})
        elif self.path == "/tailscale/enable":
            payload = tailscale_set_enabled(True)
            self._respond(200 if payload.get("ok") else 500, payload)
        elif self.path == "/tailscale/disable":
            payload = tailscale_set_enabled(False)
            self._respond(200 if payload.get("ok") else 500, payload)
        elif self.path == "/tailscale/authkey":
            body = self._read_json_body()
            authkey = (body.get("authKey") or body.get("auth_key") or "").strip()
            if not authkey:
                self._respond(400, {"ok": False, "error": "auth key required"})
                return
            payload = tailscale_set_authkey(authkey)
            self._respond(200 if payload.get("ok") else 500, payload)
        else:
            self._respond(404, {"error": "not found"})

    def do_OPTIONS(self):
        self.send_response(204)
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.end_headers()


if __name__ == "__main__":
    zc, zc_info = start_mdns()
    server = HTTPServer(("0.0.0.0", 5002), Handler)
    print(f"CommaView API v{VERSION} listening on :5002")
    try:
        server.serve_forever()
    finally:
        if zc and zc_info:
            zc.unregister_service(zc_info)
            zc.close()
APIEOF
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
copy_or_embed_api_script

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
TAILSCALE_BIN=/data/commaview/tailscale/bin/tailscale
TAILSCALED_BIN=/data/commaview/tailscale/bin/tailscaled
TAILSCALE_SOCKET=/data/commaview/tailscale/state/tailscaled.sock
TAILSCALE_STATE_FILE=/data/commaview/tailscale/state/tailscaled.state
TAILSCALE_CHECK_OFFROAD_SEC=15
TAILSCALE_CHECK_ONROAD_SEC=60

BRIDGE_BACKOFF_SEQUENCE=(1 2 4 8)
BRIDGE_BACKOFF_MAX_SEC=8
BRIDGE_HEALTHY_RESET_SEC=30
bridge_backoff_index=0
bridge_backoff_sec=0
bridge_last_start_epoch=0

tailscale_next_check_epoch=0

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

is_tailscale_enabled() {
  [ "$(read_param CommaViewTailscaleEnabled)" = "1" ]
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

reset_bridge_backoff() {
  bridge_backoff_index=0
  bridge_backoff_sec=0
}

set_next_bridge_backoff() {
  bridge_backoff_sec="${BRIDGE_BACKOFF_SEQUENCE[$bridge_backoff_index]}"
  if [ "$bridge_backoff_sec" -gt "$BRIDGE_BACKOFF_MAX_SEC" ]; then
    bridge_backoff_sec="$BRIDGE_BACKOFF_MAX_SEC"
  fi
  if [ "$bridge_backoff_index" -lt "$(( ${#BRIDGE_BACKOFF_SEQUENCE[@]} - 1 ))" ]; then
    bridge_backoff_index=$((bridge_backoff_index + 1))
  fi
}

ensure_bridge_running_prod() {
  if is_running_pidfile bridge; then
    local pid cmd now arg_count
    pid="$(cat "$RUN_DIR/bridge.pid" 2>/dev/null || true)"
    cmd="$(tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null || true)"
    arg_count="$(tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null | sed '/^$/d' | wc -l | tr -d ' ')"
    if ! echo "$cmd" | grep -Fq -- '/data/commaview/commaview-bridge'; then
      log "bridge pidfile command mismatch; restarting"
      stop_pidfile bridge
    elif [ "${arg_count:-0}" -gt 1 ]; then
      log "bridge has unexpected extra arguments; restarting prod-only"
      stop_pidfile bridge
    else
      now="$(date +%s)"
      if [ "$bridge_last_start_epoch" -gt 0 ] && [ $((now - bridge_last_start_epoch)) -ge "$BRIDGE_HEALTHY_RESET_SEC" ] && { [ "$bridge_backoff_sec" -ne 0 ] || [ "$bridge_backoff_index" -ne 0 ]; }; then
        log "bridge healthy for ${BRIDGE_HEALTHY_RESET_SEC}s; resetting restart backoff"
        reset_bridge_backoff
      fi
      return 0
    fi
  fi

  if [ "$bridge_backoff_sec" -gt 0 ]; then
    log "bridge restart backoff: sleeping ${bridge_backoff_sec}s"
    sleep "$bridge_backoff_sec"
  fi

  cd /data/openpilot
  log "starting bridge (prod-only watchdog)"
  start_bg bridge nice -n 19 /data/commaview/commaview-bridge
  bridge_last_start_epoch="$(date +%s)"
  set_next_bridge_backoff
}

ensure_tailscaled_running() {
  [ -x "$TAILSCALED_BIN" ] || return 0
  if is_running_pidfile tailscaled; then
    return 0
  fi
  mkdir -p "$(dirname "$TAILSCALE_SOCKET")"
  log "starting tailscaled"
  start_bg tailscaled "$TAILSCALED_BIN" --state "$TAILSCALE_STATE_FILE" --socket "$TAILSCALE_SOCKET"
}

force_tailscale_down_and_stop() {
  if command -v tailscale >/dev/null 2>&1; then
    tailscale --socket /data/commaview/tailscale/state/tailscaled.sock down >/dev/null 2>&1 || true
  fi
  if [ -x "$TAILSCALE_BIN" ]; then
    "$TAILSCALE_BIN" --socket "$TAILSCALE_SOCKET" down >/dev/null 2>&1 || true
  fi
  stop_pidfile tailscaled
  pkill -f '/data/commaview/tailscale/bin/tailscaled' 2>/dev/null || true
}

ensure_tailscale_up() {
  [ -x "$TAILSCALE_BIN" ] || return 0
  if ! "$TAILSCALE_BIN" --socket "$TAILSCALE_SOCKET" status --json 2>/dev/null | grep -q '"BackendState":"Running"'; then
    "$TAILSCALE_BIN" --socket "$TAILSCALE_SOCKET" up >/dev/null 2>&1 || true
  fi
}

ensure_tailscale_policy() {
  local now cadence
  now="$(date +%s)"
  if is_onroad; then
    cadence="$TAILSCALE_CHECK_ONROAD_SEC"
  else
    cadence="$TAILSCALE_CHECK_OFFROAD_SEC"
  fi

  if [ "$now" -lt "$tailscale_next_check_epoch" ]; then
    return 0
  fi
  tailscale_next_check_epoch=$((now + cadence))

  if is_onroad; then
    log "tailscale policy: onroad -> force down"
    force_tailscale_down_and_stop
    return 0
  fi

  if is_tailscale_enabled; then
    log "tailscale policy: offroad+enabled -> ensure running"
    ensure_tailscaled_running
    ensure_tailscale_up
  else
    log "tailscale policy: offroad+disabled -> force down"
    force_tailscale_down_and_stop
  fi
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
    tailscale_next_check_epoch=0
  fi

  ensure_bridge_running_prod
  ensure_tailscale_policy
  sleep 1
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

# Launch local API for app/status/tailscale control
if [ -f /data/commaview/commaview-api.py ]; then
  nohup python3 /data/commaview/commaview-api.py >> "$LOG/commaview-api.log" 2>&1 &
  echo $! > "$RUN/commaview_api.pid"
fi

# Launch tailscale guardian (policy daemon)
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
for f in bridge.pid commaview_api.pid tailscaled.pid; do
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
pkill -f 'commaview-api.py' 2>/dev/null || true
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
  "$INSTALL_DIR/commaview-api.py" \
  "$INSTALL_DIR/uninstall.sh" \
  "$INSTALL_DIR/upgrade.sh" \
  "$INSTALL_DIR/tailscale/install_tailscale_runtime.sh" \
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
echo "  Supervisor:  single-process bridge watchdog + tailscale policy"
echo "  Bridge:      prod-only"
echo "  Runtime:     openpilot manager owns camerad/encoderd"
if [ "$ENABLE_TAILSCALE" = "1" ]; then
  echo "  Tailscale:   opt-in flow complete (see /data/params/d/CommaViewTailscaleEnabled)"
fi
echo "  Upgrade:     bash $INSTALL_DIR/upgrade.sh"
echo "  Uninstall:   bash $INSTALL_DIR/uninstall.sh"
