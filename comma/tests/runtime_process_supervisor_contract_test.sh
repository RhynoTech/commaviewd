#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
START="$ROOT/comma/start.sh"
STOP="$ROOT/comma/stop.sh"

require_literal() {
  local file="$1"
  local needle="$2"
  if ! grep -Fq "$needle" "$file"; then
    echo "missing literal in ${file##*/}: $needle" >&2
    exit 1
  fi
}

require_literal "$START" 'append_runtime_run_event'
require_literal "$START" 'start_runtime_process'
require_literal "$START" 'runtime-run-events.jsonl'
require_literal "$START" 'process_launch'
require_literal "$START" 'process_exit'
require_literal "$START" 'component'
require_literal "$START" 'exitStatus'
require_literal "$START" 'restartReason'
require_literal "$START" 'bridge-supervisor.pid'
require_literal "$START" 'control-supervisor.pid'
require_literal "$START" 'COMMAVIEWD_RESTART_REASON'
require_literal "$START" 'nohup env'
require_literal "$START" 'start_runtime_process bridge bridge.pid bridge-supervisor.pid'
require_literal "$START" 'start_runtime_process control control.pid control-supervisor.pid'

# A crash restarts after a doubling backoff, a bounded number of times in a row, and only while
# the supervisor still owns its pid file (stop.sh and install remove it first).
require_literal "$START" 'supervisor_owns "$supervisor_pid_file" || exit'
require_literal "$START" 'process_restart_scheduled'
require_literal "$START" 'process_restart_gave_up'
require_literal "$START" 'COMMAVIEWD_SUPERVISOR_MAX_RESTARTS'
require_literal "$START" 'sleep "$backoff"'
require_literal "$START" 'backoff=$((backoff * 2))'
# Maintenance queued while onroad is resumed after a reboot.
require_literal "$START" 'runner.sh" resume'

require_literal "$STOP" 'bridge-supervisor.pid'
require_literal "$STOP" 'control-supervisor.pid'

if grep -Fq 'while true; do' "$START" | grep -Fq 'commaviewd'; then
  echo "runtime process supervisor must not restart commaviewd in a tight loop" >&2
  exit 1
fi

echo "PASS: runtime process supervisor contract"
