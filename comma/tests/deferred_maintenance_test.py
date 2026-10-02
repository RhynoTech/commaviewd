"""run_when_offroad.sh: maintenance asked for while onroad waits for openpilot to be offroad, and
never asks openpilot to go offroad itself."""

import json
import os
import pathlib
import subprocess
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "comma" / "scripts" / "run_when_offroad.sh"


def env_for(tmp_path):
    env = dict(os.environ)
    env.update(
        COMMAVIEWD_DEFERRED_DIR=str(tmp_path / "deferred"),
        COMMAVIEWD_PARAMS_DIR=str(tmp_path / "params"),
        COMMAVIEWD_DEFERRED_POLL_SEC="1",
        COMMAVIEWD_DEFERRED_OFFROAD_STABLE_SEC="2",
        COMMAVIEWD_DEFERRED_REQUIRE_MANAGER="0",
    )
    (tmp_path / "params").mkdir(exist_ok=True)
    return env


def run(env, *args, check=True):
    return subprocess.run(["bash", str(RUNNER), *args], env=env, capture_output=True, text=True, check=check, timeout=30)


def status(env):
    return json.loads(run(env, "status").stdout)


def wait_for(predicate, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.2)
    return False


def set_offroad(tmp_path, value):
    (tmp_path / "params" / "IsOffroad").write_text(value)


def test_job_waits_for_offroad_then_runs_once(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    marker = tmp_path / "ran"
    script = tmp_path / "job.sh"
    script.write_text(f'#!/usr/bin/env bash\necho "$COMMAVIEWD_DEFERRED_JOB $1" >> "{marker}"\n')
    try:
        queued = json.loads(run(env, "queue", "install", "--file", str(script), "--", "bash", "@JOB@/job.sh", "v9").stdout)
        assert queued["state"] == "waiting" and queued["action"] == "install" and queued["waitsFor"] == "offroad"
        time.sleep(3.5)
        assert not marker.exists(), "ran while onroad"
        assert status(env)["state"] == "waiting"
        # The queued copy runs, not the original (an install replaces the scripts under it).
        script.write_text("#!/usr/bin/env bash\nexit 9\n")
        set_offroad(tmp_path, "1")
        assert wait_for(lambda: status(env)["state"] == "done")
        assert marker.read_text() == "1 v9\n"
        final = status(env)
        assert final["exitStatus"] == 0 and final["attempts"] == 1
        assert not (tmp_path / "deferred" / "job").exists()
        # Never asked openpilot to go offroad.
        assert not (tmp_path / "params" / "OffroadMode").exists()
    finally:
        run(env, "cancel", check=False)


def test_blink_offroad_is_not_enough(tmp_path):
    env = env_for(tmp_path)
    env["COMMAVIEWD_DEFERRED_OFFROAD_STABLE_SEC"] = "4"
    marker = tmp_path / "ran"
    set_offroad(tmp_path, "1")
    try:
        run(env, "queue", "repair", "--", "touch", str(marker))
        time.sleep(1.5)
        set_offroad(tmp_path, "0")
        time.sleep(2.5)
        assert not marker.exists()
        set_offroad(tmp_path, "1")
        assert wait_for(lambda: marker.exists())
    finally:
        run(env, "cancel", check=False)


def test_unknown_road_state_never_runs(tmp_path):
    env = env_for(tmp_path)
    marker = tmp_path / "ran"
    try:
        run(env, "queue", "repair", "--", "touch", str(marker))
        time.sleep(3.5)
        assert not marker.exists(), "a missing IsOffroad must not count as offroad"
    finally:
        run(env, "cancel", check=False)


def test_requeue_replaces_and_cancel_drops(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    first, second = tmp_path / "first", tmp_path / "second"
    run(env, "queue", "install", "--", "touch", str(first))
    run(env, "queue", "uninstall", "--", "touch", str(second))
    assert status(env)["action"] == "uninstall"
    run(env, "cancel")
    assert status(env)["state"] == "cancelled"
    set_offroad(tmp_path, "1")
    time.sleep(3.5)
    assert not first.exists() and not second.exists()


def test_onroad_again_at_start_goes_back_to_waiting(tmp_path):
    env = env_for(tmp_path)
    counter = tmp_path / "count"
    script = tmp_path / "job.sh"
    # Exits 42 (blocked while onroad) the first time, succeeds the second.
    script.write_text(f'#!/usr/bin/env bash\necho x >> "{counter}"\n[ "$(wc -l < "{counter}")" -ge 2 ] || exit 42\n')
    set_offroad(tmp_path, "1")
    try:
        run(env, "queue", "install", "--file", str(script), "--", "bash", "@JOB@/job.sh")
        assert wait_for(lambda: status(env)["state"] == "done", timeout=30)
        assert status(env)["attempts"] == 2
    finally:
        run(env, "cancel", check=False)


def test_resume_restarts_the_waiter_after_a_reboot(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    marker = tmp_path / "ran"
    run(env, "queue", "install", "--", "touch", str(marker))
    pid = int((tmp_path / "deferred" / "waiter.pid").read_text())
    os.kill(pid, 9)  # what a reboot does to it
    time.sleep(0.3)
    try:
        run(env, "resume")
        set_offroad(tmp_path, "1")
        assert wait_for(lambda: marker.exists())
    finally:
        run(env, "cancel", check=False)


def test_scripts_never_force_offroad():
    for rel in ("install.sh", "uninstall.sh", "scripts/apply_onroad_ui_export_patch.sh",
                "scripts/revert_onroad_ui_export_patch.sh", "scripts/run_when_offroad.sh", "start.sh"):
        text = (REPO_ROOT / "comma" / rel).read_text()
        assert 'write_param "OffroadMode" "1"' not in text, rel


def shell_function(text, name):
    import re
    match = re.search(r"^" + re.escape(name) + r"\(\) \{\n.*?^\}\n", text, re.S | re.M)
    assert match, name
    return match.group(0)


def test_install_while_onroad_refuses_or_queues_never_forces(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    companions = tmp_path / "companions"
    (companions / "scripts").mkdir(parents=True)
    (companions / "install.sh").write_text("#!/usr/bin/env bash\necho installing \"$@\"\n")
    (companions / "scripts" / "run_when_offroad.sh").write_text(RUNNER.read_text())
    text = (REPO_ROOT / "comma" / "install.sh").read_text()
    functions = "".join(shell_function(text, name) for name in (
        "read_is_onroad", "queue_install_until_offroad", "cancel_deferred_maintenance", "ensure_offroad_ready"))

    def ensure(force):
        script = f"""
set -euo pipefail
PARAMS_DIR={tmp_path / 'params'}
read_param() {{ [ -f "$PARAMS_DIR/$1" ] && tr -d '\\000\\r\\n' < "$PARAMS_DIR/$1" || true; }}
COMPANION_DIR={companions}
DEFERRED_DIR={tmp_path / 'deferred'}
DEFERRED_EXIT=75
RELEASE_TAG=v9.9.9
FORCE_OFFROAD={1 if force else 0}
{functions}
ensure_offroad_ready
echo proceeding
"""
        return subprocess.run(["bash", "-c", script], env=env, capture_output=True, text=True, timeout=30)

    try:
        refused = ensure(False)
        assert refused.returncode == 42, refused.stdout + refused.stderr
        assert "proceeding" not in refused.stdout
        queued = ensure(True)
        assert queued.returncode == 75, queued.stdout + queued.stderr
        assert "COMMAVIEW_MAINTENANCE_DEFERRED=install" in queued.stdout
        assert status(env)["state"] == "waiting" and status(env)["action"] == "install"
        args = (tmp_path / "deferred" / "job" / "args").read_bytes().split(b"\0")
        assert args[:4] == [b"bash", str(tmp_path / "deferred" / "job" / "install.sh").encode(), b"--tag", b"v9.9.9"]
        assert not (tmp_path / "params" / "OffroadMode").exists()
        # Run directly once offroad, the install drops the queued one.
        set_offroad(tmp_path, "1")
        direct = ensure(True)
        assert direct.returncode == 0 and "proceeding" in direct.stdout
        assert status(env)["state"] == "cancelled"
    finally:
        run(env, "cancel", check=False)


def test_uninstall_while_onroad_refuses_or_queues(tmp_path):
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    install_dir = tmp_path / "commaview"
    (install_dir / "scripts").mkdir(parents=True)
    (install_dir / "scripts" / "run_when_offroad.sh").write_text(RUNNER.read_text())
    (install_dir / "uninstall.sh").write_text((REPO_ROOT / "comma" / "uninstall.sh").read_text())
    stop_marker = install_dir / "stopped"
    (install_dir / "stop.sh").write_text(f"#!/usr/bin/env bash\ntouch {stop_marker}\n")
    env["COMMAVIEWD_INSTALL_DIR"] = str(install_dir)
    try:
        refused = subprocess.run(["bash", str(install_dir / "uninstall.sh")], env=env, capture_output=True, text=True, timeout=30)
        assert refused.returncode == 42 and "blocked while onroad" in refused.stderr
        queued = subprocess.run(["bash", str(install_dir / "uninstall.sh"), "--force-offroad"], env=env,
                                capture_output=True, text=True, timeout=30)
        assert queued.returncode == 75, queued.stdout + queued.stderr
        assert "COMMAVIEW_MAINTENANCE_DEFERRED=uninstall" in queued.stdout
        assert status(env)["action"] == "uninstall"
        assert install_dir.exists() and not stop_marker.exists()
        assert not (tmp_path / "params" / "OffroadMode").exists()
    finally:
        run(env, "cancel", check=False)


def test_a_direct_run_clears_a_finished_jobs_status(tmp_path):
    # The app shows a failed or finished job until something replaces it; a direct install,
    # uninstall or repair calls cancel, which must not leave the old job's status behind.
    env = env_for(tmp_path)
    set_offroad(tmp_path, "0")
    run(env, "queue", "install", "--", "false")
    set_offroad(tmp_path, "1")
    try:
        assert wait_for(lambda: status(env)["state"] == "failed"), status(env)
    finally:
        run(env, "cancel", check=False)
    assert status(env) == {"state": "none"}
