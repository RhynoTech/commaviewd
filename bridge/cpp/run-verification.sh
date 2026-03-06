#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/home/pear/openpilot-src bridge/cpp/run-verification.sh
Runs phase-2 verification pipeline: reproducible build + unit tests.
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "$0")" && pwd)"
OP_ROOT="${OP_ROOT:-/home/pear/openpilot-src}"

OP_ROOT="$OP_ROOT" "$ROOT/reproducible-build.sh"
OP_ROOT="$OP_ROOT" "$ROOT/run-unit-tests.sh"

echo "PASS: bridge verification pipeline complete"
