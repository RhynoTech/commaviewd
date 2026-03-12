#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/home/pear/openpilot-src commaviewd/scripts/run-verification.sh
Runs reproducible build + unit test pipeline.
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OP_ROOT="${OP_ROOT:-/home/pear/openpilot-src}"

OP_ROOT="$OP_ROOT" "$ROOT/scripts/reproducible-build.sh"
OP_ROOT="$OP_ROOT" "$ROOT/scripts/run-unit-tests.sh"

echo "PASS: commaviewd verification pipeline complete"
