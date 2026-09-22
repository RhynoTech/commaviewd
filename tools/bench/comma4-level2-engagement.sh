#!/usr/bin/env bash
set -euo pipefail

OPENPILOT_ROOT="${OPENPILOT_ROOT:-/data/openpilot-dev}"
RESULTS="${COMMAVIEW_BENCH_RESULTS:-/data/commaview-bench/results}"
HARNESS="${COMMAVIEW_LEVEL2_HARNESS:-/data/commaview-bench/bin/comma4-level2-engagement.py}"

is_offroad="$(tr -d '\000\r\n' < /data/params/d/IsOffroad 2>/dev/null || true)"
is_engaged="$(tr -d '\000\r\n' < /data/params/d/IsEngaged 2>/dev/null || true)"
[[ "$is_offroad" == "1" && "$is_engaged" != "1" ]] || {
  echo "ERROR: production device is not safely offroad" >&2
  exit 42
}

mkdir -p "$RESULTS" /data/commaview-bench/level2-params
for mode in baseline thread; do
  prefix="cvlevel2-${mode}"
  socket_path="/tmp/${prefix}-ui-export.sock"
  result="$RESULTS/level2-${mode}.json"
  rm -rf "/dev/shm/msgq_${prefix}" "/data/commaview-bench/level2-params/${prefix}"
  mkdir -p "/dev/shm/msgq_${prefix}"
  echo "--- level2 $mode ---"
  OPENPILOT_PREFIX="$prefix" \
  PARAMS_ROOT=/data/commaview-bench/level2-params \
  PYTHONPATH="$OPENPILOT_ROOT:$OPENPILOT_ROOT/msgq_repo:$OPENPILOT_ROOT/opendbc_repo" \
    "$OPENPILOT_ROOT/.venv/bin/python" "$HARNESS" --mode "$mode" --socket-path "$socket_path" \
    | tee "$result"
done

echo "LEVEL2 ALL PASSED"
