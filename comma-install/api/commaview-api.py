#!/usr/bin/env python3
"""CommaView Management API - runs on comma device port 5002"""
import hmac
import json
import os
import socket
import subprocess
import threading
import time
from http.server import HTTPServer, BaseHTTPRequestHandler

VERSION = "0.1.2-alpha"
INSTALL_DIR = "/data/commaview"
TAILSCALECTL = f"{INSTALL_DIR}/tailscale/tailscalectl.sh"

MDNS_SERVICE_TYPE = "_commaview._tcp.local."
MDNS_REFRESH_SEC = 15
API_TOKEN_FILE = os.getenv("COMMAVIEW_API_TOKEN_FILE", f"{INSTALL_DIR}/api/auth.token")


def load_api_token():
    direct = (os.getenv("COMMAVIEW_API_TOKEN") or "").strip()
    if direct:
        return direct

    try:
        with open(API_TOKEN_FILE, "r") as f:
            token = f.read().strip()
            return token or None
    except Exception:
        return None


API_TOKEN = load_api_token()


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


def _instance_name():
    dongle_id = get_dongle_id()
    suffix = dongle_id[-6:] if dongle_id and dongle_id != "unknown" else socket.gethostname()[-6:]
    return f"comma-{suffix}"


def _build_mdns_info(ServiceInfo, ip):
    instance_name = _instance_name()
    return ServiceInfo(
        MDNS_SERVICE_TYPE,
        f"{instance_name}.{MDNS_SERVICE_TYPE}",
        addresses=[socket.inet_aton(ip)],
        port=5002,
        properties={
            "version": VERSION,
            "api_port": "5002",
            "road_port": "8200",
            "wide_port": "8201",
            "driver_port": "8202",
        },
    )


def start_mdns():
    """Advertise CommaView via mDNS/Zeroconf so Android app can auto-discover."""
    try:
        from zeroconf import Zeroconf, ServiceInfo
    except ImportError:
        print("[mDNS] zeroconf not installed, skipping mDNS advertisement")
        print("[mDNS] Install with: pip install zeroconf")
        return None

    ip = get_ip()
    if ip == "unknown":
        print("[mDNS] No IP address, skipping mDNS")
        return None

    zc = Zeroconf()
    info = _build_mdns_info(ServiceInfo, ip)
    zc.register_service(info)
    print(f"[mDNS] Advertising on {ip}")
    return {
        "zc": zc,
        "info": info,
        "ip": ip,
        "ServiceInfo": ServiceInfo,
        "lock": threading.Lock(),
    }


def refresh_mdns_if_ip_changed(state):
    if not state:
        return

    ip = get_ip()
    if ip == "unknown" or ip == state.get("ip"):
        return

    with state["lock"]:
        old_info = state.get("info")
        zc = state.get("zc")
        ServiceInfo = state.get("ServiceInfo")
        if not zc or not ServiceInfo:
            return

        try:
            if old_info:
                zc.unregister_service(old_info)
        except Exception:
            pass

        try:
            new_info = _build_mdns_info(ServiceInfo, ip)
            zc.register_service(new_info)
            state["info"] = new_info
            state["ip"] = ip
            print(f"[mDNS] Re-advertised on IP change: {ip}")
        except Exception as e:
            print(f"[mDNS] Failed to refresh registration: {e}")


def mdns_refresh_loop(state, stop_event):
    while not stop_event.wait(MDNS_REFRESH_SEC):
        refresh_mdns_if_ip_changed(state)


def stop_mdns(state):
    if not state:
        return

    zc = state.get("zc")
    info = state.get("info")
    if not zc:
        return

    try:
        if info:
            zc.unregister_service(info)
    except Exception:
        pass

    try:
        zc.close()
    except Exception:
        pass


class Handler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass  # silence logs

    def _respond(self, code, data):
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-CommaView-Token")
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())

    def _authorize_post(self):
        if not API_TOKEN:
            return True

        token = (self.headers.get("X-CommaView-Token") or "").strip()
        if token and hmac.compare_digest(token, API_TOKEN):
            return True

        self._respond(401, {"ok": False, "error": "unauthorized"})
        return False

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
                "authRequired": bool(API_TOKEN),
                "services": {
                    "commaview_supervisor": is_running("commaview-supervisor.sh"),
                    "commaview_bridge": is_running("/data/commaview/commaview-bridge"),
                    "commaview_api": True,
                },
                "api_port": 5002,
                "video_ports": {
                    "road": 8200,
                    "wide": 8201,
                    "driver": 8202,
                },
                "tailscale": tailscale_status(),
            })
        elif self.path == "/commaview/version":
            self._respond(200, {"version": VERSION})
        elif self.path == "/tailscale/status":
            self._respond(200, tailscale_status())
        else:
            self._respond(404, {"error": "not found"})

    def do_POST(self):
        if not self._authorize_post():
            return

        if self.path == "/tailscale/enable":
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
        self.send_header("Access-Control-Allow-Headers", "Content-Type, X-CommaView-Token")
        self.end_headers()


if __name__ == "__main__":
    mdns_state = start_mdns()
    mdns_stop = threading.Event()
    mdns_thread = None
    if mdns_state:
        mdns_thread = threading.Thread(target=mdns_refresh_loop, args=(mdns_state, mdns_stop), daemon=True)
        mdns_thread.start()

    server = HTTPServer(("0.0.0.0", 5002), Handler)
    print(f"CommaView API v{VERSION} listening on :5002")
    try:
        server.serve_forever()
    finally:
        mdns_stop.set()
        if mdns_thread:
            mdns_thread.join(timeout=1.0)
        stop_mdns(mdns_state)
