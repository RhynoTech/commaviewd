#!/usr/bin/env python3
"""Exercise the paired, offroad-only Wi-Fi control without touching host Wi-Fi."""

import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def request(port, method="GET", token=None, body=None):
    data = json.dumps(body).encode() if body is not None else None
    headers = {"Content-Type": "application/json"}
    if token:
        headers["X-CommaView-Token"] = token
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}/commaview/wifi/power-save",
        data=data,
        headers=headers,
        method=method,
    )
    try:
        with urllib.request.urlopen(req, timeout=2) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def main():
    binary = Path(os.environ.get("COMMAVIEWD_HOST_BIN", "")) if os.environ.get("COMMAVIEWD_HOST_BIN") else \
        Path(__file__).resolve().parents[2] / "dist/commaviewd-host"
    if not binary.is_file():
        raise SystemExit("build host runtime first")
    with tempfile.TemporaryDirectory(prefix="commaview-wifi-test-") as temp:
        root = Path(temp)
        tools = root / "bin"
        tools.mkdir()
        params = root / "params"
        params.mkdir()
        (params / "IsOffroad").write_text("1")
        (params / "IsEngaged").write_text("0")
        (root / "profile").write_text("lab test\n")
        (root / "profile_mode").write_text("enable\n")
        (tools / "nmcli").write_text(
            "#!/bin/sh\n"
            'if [ "$1" = "-g" ] && [ "$2" = "GENERAL.CONNECTION" ]; '
            'then cat "$STATE_DIR/profile"; exit 0; fi\n'
            'if [ "$1" = "-g" ] && [ "$2" = "802-11-wireless.powersave" ]; '
            'then cat "$STATE_DIR/profile_mode"; exit 0; fi\n'
            'if [ "$1" = "connection" ] && [ "$2" = "modify" ] && '
            '[ "$3" = "id" ] && [ "$4" = "lab test" ] && '
            '[ "$5" = "802-11-wireless.powersave" ]; then '
            'if [ "$6" = "2" ]; then echo disable > "$STATE_DIR/profile_mode"; '
            'else echo enable > "$STATE_DIR/profile_mode"; fi; exit 0; fi\n'
            "exit 1\n"
        )
        for tool in tools.iterdir():
            tool.chmod(0o755)
        with socket.socket() as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        env = dict(os.environ, PATH=f"{tools}:{os.environ['PATH']}", STATE_DIR=str(root),
                   COMMAVIEWD_TEST_PARAMS_DIR=str(params), COMMAVIEWD_API_TOKEN="test-token")
        process = subprocess.Popen([str(binary), "control", "--port", str(port)],
                                   env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            for _ in range(50):
                try:
                    request(port, token="test-token")
                    break
                except (OSError, ValueError):
                    time.sleep(0.1)
            assert request(port)[0] == 401
            assert request(port, token="test-token") == (200, {"ok": True, "mode": "enable", "enabled": True})
            (root / "profile_mode").write_text("default\n")
            assert request(port, token="test-token") == (200, {"ok": True, "mode": "default", "enabled": None})
            assert request(port, "POST", "test-token", {"mode": "off"}) == (200, {"ok": True, "mode": "disable", "enabled": False, "reconnectRequired": True})
            assert (root / "profile_mode").read_text().strip() == "disable"
            (params / "IsOffroad").write_text("0")
            assert request(port, "POST", "test-token", {"mode": "on"})[0] == 403
            (params / "IsOffroad").write_text("1")
            (params / "IsEngaged").write_text("1")
            assert request(port, "POST", "test-token", {"mode": "on"})[0] == 403
            assert (root / "profile_mode").read_text().strip() == "disable"
            (params / "IsEngaged").write_text("0")
            assert request(port, "POST", "test-token", {"mode": "on"}) == (200, {"ok": True, "mode": "enable", "enabled": True, "reconnectRequired": True})
            assert (root / "profile_mode").read_text().strip() == "enable"
            print("PASS: paired Wi-Fi control saves active profile and rejects onroad/engaged changes")
        finally:
            process.terminate()
            process.wait(timeout=5)


if __name__ == "__main__":
    main()
