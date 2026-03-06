#!/usr/bin/env python3
"""CommaView Management API - runs on comma device port 5002"""
import json
import os
import socket
import subprocess
from http.server import HTTPServer, BaseHTTPRequestHandler

VERSION = "0.1.2-alpha"
INSTALL_DIR = "/data/commaview"
TAILSCALECTL = f"{INSTALL_DIR}/tailscale/tailscalectl.sh"

MDNS_SERVICE_TYPE = "_commaview._tcp.local."


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
            "api_port": "5002",
            "road_port": "8200",
            "wide_port": "8201",
            "driver_port": "8202",
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
