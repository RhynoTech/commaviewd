#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
START="$ROOT/comma4/start.sh"
CONTROL="$ROOT/commaviewd/src/control_mode.cpp"

require_literal() {
  local file="$1"
  local needle="$2"
  if ! grep -Fq "$needle" "$file"; then
    echo "missing literal in ${file##*/}: $needle" >&2
    exit 1
  fi
}

require_literal "$START" 'rotate_runtime_log_if_large'
require_literal "$START" 'prune_runtime_logs_by_age'
require_literal "$START" 'prune_runtime_logs_by_total_size'
require_literal "$START" 'rotate_runtime_logs'
require_literal "$START" 'start_runtime_log_rotation_loop'
require_literal "$START" 'COMMAVIEWD_LOG_MAX_BYTES'
require_literal "$START" 'COMMAVIEWD_LOG_MAX_FILES_PER_LOG'
require_literal "$START" 'COMMAVIEWD_LOG_MAX_AGE_DAYS'
require_literal "$START" 'COMMAVIEWD_LOG_TOTAL_MAX_BYTES'
require_literal "$START" 'COMMAVIEWD_LOG_ROTATE_INTERVAL_SEC'
require_literal "$START" 'COMMAVIEWD_LOG_MAX_AGE_DAYS:-14'
require_literal "$START" '268435456'
require_literal "$START" 'log-rotation.pid'
require_literal "$START" 'commaviewd-bridge.log'
require_literal "$START" 'commaviewd-control.log'
require_literal "$START" 'onroad-ui-export-startup.log'
require_literal "$START" 'runtime-run-events.jsonl'
require_literal "$START" 'COMMAVIEWD_LOG_MAX_FILES_PER_LOG:-14'
require_literal "$START" 'mv -f "$log_file.$idx" "$log_file.$next_idx"'
require_literal "$START" 'find "$LOG_DIR" -type f'
require_literal "$START" 'sort -n'

require_literal "$CONTROL" '\"rotated\"'
require_literal "$CONTROL" 'rotated_name = name + "." + std::to_string(idx)'
require_literal "$CONTROL" 'idx <= 14'

if grep -Fq 'journalctl' "$START" || grep -Fq 'dmesg' "$START"; then
  echo "runtime log rotation must not touch journalctl/dmesg" >&2
  exit 1
fi

echo "PASS: runtime log rotation contract"
