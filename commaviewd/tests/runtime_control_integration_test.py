#!/usr/bin/env python3
"""Control API, end to end, for the things that must never disturb a drive:

- runtime-debug apply reloads the running bridge's config in place (SIGHUP): same process, no
  restart, the new policy in its effective config;
- /commaview/status reports roadPhase offroad / parked / driving from carState and selfdriveState
  read without subscribing, next to the unchanged roadState;
- a Safe Repair asked for while onroad is queued until openpilot is offroad (202, deferred), never
  forced, and the queue shows in /commaview/status.
"""

import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import tempfile
import time
import urllib.error
import urllib.request

REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "comma" / "scripts" / "run_when_offroad.sh"
TOKEN = "test-token"


def request(port, path, method="GET", body=None, token=TOKEN):
    data = json.dumps(body, separators=(",", ":")).encode() if body is not None else (b"" if method == "POST" else None)
    headers = {"X-CommaView-Token": token, "Content-Type": "application/json"}
    req = urllib.request.Request(f"http://127.0.0.1:{port}{path}", data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as response:
            return response.status, json.load(response)
    except urllib.error.HTTPError as error:
        return error.code, json.load(error)


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_until(predicate, timeout=10.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.1)
    return False


def start_control(binary, env, port):
    process = subprocess.Popen([str(binary), "control", "--port", str(port)],
                               env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def up():
        try:
            request(port, "/commaview/version")
            return True
        except (OSError, ValueError):
            return False
    if not wait_until(up):
        process.kill()
        raise SystemExit("control API did not start")
    return process


def main():
    binary = Path(os.environ.get("COMMAVIEWD_HOST_BIN") or REPO_ROOT / "dist/commaviewd-host")
    road_tool = os.environ.get("COMMAVIEWD_ROAD_QUEUE_TOOL")
    if not binary.is_file():
        raise SystemExit("build host runtime first")

    with tempfile.TemporaryDirectory(prefix="commaview-runtime-control-test-") as temp:
        root = Path(temp)
        run, config, params, msgq, deferred = (root / name for name in ("run", "config", "params", "shm", "deferred"))
        for folder in (run, config, params, msgq, root / "data"):
            folder.mkdir()
        (params / "IsOffroad").write_text("1")
        env = dict(os.environ)
        env.update(
            COMMAVIEWD_API_TOKEN=TOKEN,
            COMMAVIEWD_RUNTIME_DEBUG_CONFIG=str(config / "runtime-debug.json"),
            COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS=str(REPO_ROOT / "comma" / "runtime-debug.defaults.json"),
            COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE=str(run / "runtime-debug-effective.json"),
            COMMAVIEWD_RUNTIME_STATS=str(run / "telemetry-stats.json"),
            COMMAVIEWD_TEST_PARAMS_DIR=str(params),
            COMMAVIEWD_TEST_MSGQ_DIR=str(msgq),
            COMMAVIEWD_TEST_DATA_ROOT=str(root / "data"),
            COMMAVIEWD_DEFERRED_DIR=str(deferred),
            COMMAVIEWD_TEST_DEFERRED_RUNNER=str(RUNNER),
            COMMAVIEWD_DEFERRED_REQUIRE_MANAGER="0",
            COMMAVIEWD_PARAMS_DIR=str(params),
        )

        bridge = subprocess.Popen([str(binary), "bridge"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        (run / "bridge.pid").write_text(f"{bridge.pid}\n")
        port = free_port()
        control = start_control(binary, env, port)
        try:
            stats_path = run / "telemetry-stats.json"
            assert wait_until(lambda: stats_path.exists()), "bridge wrote no stats"

            # --- runtime-debug apply: in place, no restart -------------------------------------
            code, body = request(port, "/commaview/runtime-debug/config")
            assert code == 200, body
            config_body = {"configVersion": 1, "instrumentationLevel": "standard",
                           "services": {"carState": {"mode": "sample", "sampleHz": 2}}}
            code, body = request(port, "/commaview/runtime-debug/config", "POST", config_body)
            assert code == 200 and body["ok"], body
            code, body = request(port, "/commaview/runtime-debug/apply", "POST", {})
            assert code == 200, body
            assert body["ok"] and body["applied"] and body["appliedLive"], body
            assert body["restartScheduled"] is False and body["appliesWhenParked"] == [], body
            assert bridge.poll() is None, "the bridge must not restart"
            effective = json.loads((run / "runtime-debug-effective.json").read_text())
            assert effective["services"]["carState"]["mode"] == "sample", effective
            stats = json.loads(stats_path.read_text())
            assert stats["configReloads"] == 1, stats
            assert stats["videoRing"]["msgqReaderSlots"] == 0, stats
            print("PASS: runtime-debug apply reloads the running bridge in place (same pid, no restart)")

            # A pid file naming something that isn't our bridge is never signalled.
            sleeper = subprocess.Popen(["sleep", "30"])
            (run / "bridge.pid").write_text(f"{sleeper.pid}\n")
            code, body = request(port, "/commaview/runtime-debug/apply", "POST", {})
            assert code == 200 and body["ok"] and not body["applied"] and body["appliesOnNextStart"], body
            assert sleeper.poll() is None, "an unrelated process was signalled"
            sleeper.kill()
            (run / "bridge.pid").write_text(f"{bridge.pid}\n")
            print("PASS: runtime-debug apply signals only a pid that really is the bridge")

            # --- roadPhase --------------------------------------------------------------------
            code, status = request(port, "/commaview/status")
            assert status["roadState"] == "offroad" and status["roadPhase"] == "offroad", status
            assert status["deferredMaintenance"] == {"state": "none"}, status
            (params / "IsOffroad").write_text("0")
            code, status = request(port, "/commaview/status")
            assert status["roadState"] == "onroad" and status["roadPhase"] == "driving", status
            assert status["roadPhaseReason"] == "car-state-missing", status
            if road_tool:
                def phase(gear, standstill, enabled, age_ms="0"):
                    subprocess.run([road_tool, "--write-road-queues", str(msgq), gear, standstill, enabled, age_ms], check=True)
                    return request(port, "/commaview/status")[1]
                parked = phase("park", "1", "0")
                assert parked["roadPhase"] == "parked" and parked["roadState"] == "onroad", parked
                assert phase("drive", "1", "0")["roadPhase"] == "driving"
                assert phase("unknown", "1", "0")["roadPhaseReason"] == "gear-unknown"
                assert phase("park", "1", "1")["roadPhaseReason"] == "engaged"
                assert phase("park", "1", "0", "2000")["roadPhaseReason"] == "car-state-stale"
                print("PASS: /commaview/status reports roadPhase offroad / parked / driving")
            else:
                print("SKIP: roadPhase from queues (set COMMAVIEWD_ROAD_QUEUE_TOOL)")

            # --- Safe Repair while onroad: queued, not forced ---------------------------------
            code, body = request(port, "/commaview/onroad-ui-export/repair", "POST", {"forceOffroad": True})
            assert code == 202, (code, body)
            assert body["ok"] is False and body["deferred"] is True, body
            assert body["status"]["state"] == "deferred-until-offroad", body
            assert body["deferredMaintenance"]["state"] == "waiting" and body["deferredMaintenance"]["action"] == "repair", body
            assert not (params / "OffroadMode").exists(), "nothing may ask openpilot to go offroad"
            code, status = request(port, "/commaview/status")
            assert status["deferredMaintenance"]["state"] == "waiting", status
            code, body = request(port, "/commaview/onroad-ui-export/repair", "POST", {})
            assert body["status"]["state"] == "onroad-blocked", body
            print("PASS: Safe Repair while onroad is queued until offroad, never forced")
        finally:
            subprocess.run(["bash", str(RUNNER), "cancel"], env=env, check=False, capture_output=True)
            control.terminate()
            control.wait(timeout=5)
            bridge.send_signal(signal.SIGKILL)
            bridge.wait(timeout=5)


if __name__ == "__main__":
    main()
