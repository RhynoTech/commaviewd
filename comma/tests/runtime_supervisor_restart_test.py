"""start.sh's supervisor: a crashed bridge or control process comes back after a growing backoff,
a bounded number of times; a deliberate stop, or a supervisor that no longer owns its pid file,
never restarts anything."""

import json
import os
import pathlib
import re
import signal
import subprocess
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
START_SH = REPO_ROOT / "comma" / "start.sh"


def function_definition(text, name):
    match = re.search(r"^" + re.escape(name) + r"\(\) \{\n.*?^\}\n", text, re.S | re.M)
    assert match, name
    return match.group(0)


def launch_supervisor(tmp_path, binary_body, **settings):
    text = START_SH.read_text()
    binary = tmp_path / "commaviewd"
    binary.write_text("#!/usr/bin/env bash\n" + binary_body)
    binary.chmod(0o755)
    (tmp_path / "run").mkdir()
    (tmp_path / "logs").mkdir()
    env_lines = "\n".join(f"{key}={value}" for key, value in {
        "COMMAVIEWD_SUPERVISOR_FIRST_BACKOFF_SEC": 1,
        "COMMAVIEWD_SUPERVISOR_MAX_BACKOFF_SEC": 2,
        "COMMAVIEWD_SUPERVISOR_MAX_RESTARTS": 3,
        "COMMAVIEWD_SUPERVISOR_HEALTHY_SEC": 600,
        **settings,
    }.items())
    script = f"""
set +e
RUN_DIR={tmp_path / 'run'}
LOG_DIR={tmp_path / 'logs'}
RESTART_REASON=test
COMMAVIEWD_BIN={binary}
{env_lines}
{function_definition(text, 'runtime_event_ts_ms')}
{function_definition(text, 'append_runtime_run_event')}
{function_definition(text, 'supervisor_owns')}
{function_definition(text, 'start_runtime_process')}
start_runtime_process bridge bridge.pid bridge-supervisor.pid {tmp_path / 'logs' / 'bridge.log'} bridge env
"""
    subprocess.run(["bash", "-c", script], check=True, timeout=10)
    return int((tmp_path / "run" / "bridge-supervisor.pid").read_text())


def events(tmp_path):
    path = tmp_path / "logs" / "runtime-run-events.jsonl"
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def alive(pid):
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    return True


def wait_until(predicate, timeout=20.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(0.1)
    return False


def test_crash_loop_backs_off_and_gives_up(tmp_path):
    launches = tmp_path / "launches"
    supervisor = launch_supervisor(tmp_path, f'echo "$(date +%s.%N)" >> {launches}\nkill -SEGV $$\n')
    assert wait_until(lambda: not alive(supervisor), timeout=30)
    times = [float(line) for line in launches.read_text().split()]
    assert len(times) == 4  # the first run and MAX_RESTARTS restarts, then it gives up
    gaps = [b - a for a, b in zip(times, times[1:])]
    assert gaps[0] >= 0.9 and gaps[1] >= 1.9 and gaps[2] >= 1.9, gaps  # 1 s, then 2 s (the cap)
    kinds = [e["event"] for e in events(tmp_path)]
    assert kinds.count("process_launch") == 4
    assert kinds.count("process_restart_scheduled") == 3
    assert kinds[-1] == "process_restart_gave_up"
    scheduled = [e for e in events(tmp_path) if e["event"] == "process_restart_scheduled"]
    assert [e["restartInSec"] for e in scheduled] == [1, 2, 2]
    assert all(e["exitStatus"] == 139 for e in scheduled)


def test_a_clean_exit_or_sigterm_is_not_restarted(tmp_path):
    launches = tmp_path / "launches"
    supervisor = launch_supervisor(tmp_path, f'echo x >> {launches}\nexec sleep 30\n')
    assert wait_until(lambda: (tmp_path / "run" / "bridge.pid").exists() and launches.exists())
    child = int((tmp_path / "run" / "bridge.pid").read_text())
    os.kill(child, signal.SIGTERM)
    assert wait_until(lambda: not alive(supervisor))
    time.sleep(1.5)
    assert launches.read_text().split() == ["x"]


def test_stopped_by_stop_sh_is_not_restarted(tmp_path):
    launches = tmp_path / "launches"
    supervisor = launch_supervisor(tmp_path, f'echo x >> {launches}\nexec sleep 30\n')
    assert wait_until(lambda: (tmp_path / "run" / "bridge.pid").exists() and launches.exists())
    child = int((tmp_path / "run" / "bridge.pid").read_text())
    # What stop.sh does: drop the pid files, then (after TERM is ignored) SIGKILL.
    (tmp_path / "run" / "bridge-supervisor.pid").unlink()
    os.kill(child, signal.SIGKILL)
    assert wait_until(lambda: not alive(supervisor))
    time.sleep(1.5)
    assert launches.read_text().split() == ["x"]


def test_a_crash_is_restarted_and_the_new_process_runs(tmp_path):
    launches = tmp_path / "launches"
    body = f'echo x >> {launches}\n[ "$(wc -l < {launches})" -ge 2 ] && exec sleep 30\nexit 3\n'
    supervisor = launch_supervisor(tmp_path, body)
    try:
        assert wait_until(lambda: launches.exists() and len(launches.read_text().split()) == 2)
        time.sleep(0.5)
        assert alive(supervisor)
        assert int((tmp_path / "run" / "bridge.pid").read_text()) != 0
    finally:
        (tmp_path / "run" / "bridge-supervisor.pid").unlink()
        os.kill(int((tmp_path / "run" / "bridge.pid").read_text()), signal.SIGKILL)
        wait_until(lambda: not alive(supervisor))
