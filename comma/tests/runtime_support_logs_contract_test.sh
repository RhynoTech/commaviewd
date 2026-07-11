#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CONTROL="$ROOT/commaviewd/src/control_mode.cpp"

require_literal() {
  local needle="$1"
  if ! grep -Fq "$needle" "$CONTROL"; then
    echo "missing literal: $needle" >&2
    exit 1
  fi
}

require_literal '"/commaview/support/logs"'
require_literal 'support_logs_response_json'
require_literal 'commaviewd-bridge.log'
require_literal 'commaviewd-control.log'
require_literal 'onroad-ui-export-startup.log'
require_literal 'runtime-run-events.jsonl'
require_literal 'telemetry-stats.json'
require_literal 'runtime-debug-effective.json'
require_literal 'last-restart-reason.txt'
require_literal 'kSupportLogPerFileCapBytes'
require_literal 'kSupportLogTotalCapBytes'
require_literal 'is_authorized(req, api_token)'

if grep -Fq 'journalctl' "$CONTROL" || grep -Fq 'dmesg' "$CONTROL"; then
  echo "support logs v1 must not collect journalctl/dmesg" >&2
  exit 1
fi

echo "PASS: runtime support logs endpoint contract"
