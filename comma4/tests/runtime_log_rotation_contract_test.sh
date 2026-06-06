#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
START="$ROOT/comma4/start.sh"

require_literal() {
  local needle="$1"
  if ! grep -Fq "$needle" "$START"; then
    echo "missing literal: $needle" >&2
    exit 1
  fi
}

require_literal 'rotate_runtime_log_if_large'
require_literal 'rotate_runtime_logs'
require_literal 'COMMAVIEWD_LOG_MAX_BYTES'
require_literal 'commaviewd-bridge.log'
require_literal 'commaviewd-control.log'
require_literal 'onroad-ui-export-startup.log'
require_literal 'runtime-run-events.jsonl'
require_literal '.1'
require_literal 'mv -f'

if grep -Fq 'journalctl' "$START" || grep -Fq 'dmesg' "$START"; then
  echo "runtime log rotation must not touch journalctl/dmesg" >&2
  exit 1
fi

echo "PASS: runtime log rotation contract"
