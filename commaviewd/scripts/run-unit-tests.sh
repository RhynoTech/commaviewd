#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: OP_ROOT=/path/to/openpilot-src commaviewd/scripts/run-unit-tests.sh
Compiles and runs commaviewd unit tests (framing, runtime mode, control policy, telemetry).
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
DEFAULT_OP_ROOT="$REPO_ROOT/../openpilot-src"
if [[ -d "$DEFAULT_OP_ROOT" ]]; then
  OP_ROOT="${OP_ROOT:-$DEFAULT_OP_ROOT}"
else
  OP_ROOT="${OP_ROOT:-$HOME/openpilot-src}"
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

OP_ROOT="$OP_ROOT" "$ROOT/scripts/build-ubuntu.sh" >/dev/null

CXX="${CXX:-clang++}"
INC=( -I"$ROOT/include" -I"$OP_ROOT" -I"$OP_ROOT/cereal/messaging" -I"$OP_ROOT/msgq_repo" )

$CXX -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_net_framing.cpp" \
  "$ROOT/src/framing.cpp" \
  -o "$TMP/test_net_framing"

$CXX -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_runtime_mode.cpp" \
  "$ROOT/src/mode.cpp" \
  -o "$TMP/test_runtime_mode"

$CXX -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_control_policy.cpp" \
  "$ROOT/src/policy.cpp" \
  "$ROOT/src/framing.cpp" \
  -o "$TMP/test_control_policy"

$CXX -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_telemetry_json.cpp" \
  "$ROOT/src/json_builder.cpp" \
  "$OP_ROOT/cereal/gen/cpp/log.capnp.c++" \
  "$OP_ROOT/cereal/gen/cpp/car.capnp.c++" \
  "$OP_ROOT/cereal/gen/cpp/legacy.capnp.c++" \
  "$OP_ROOT/cereal/gen/cpp/custom.capnp.c++" \
  -lcapnp -lkj -lpthread \
  -o "$TMP/test_telemetry_json"

"$TMP/test_net_framing"
"$TMP/test_runtime_mode"
"$TMP/test_control_policy"
"$TMP/test_telemetry_json"

echo "PASS: commaviewd unit tests passed"
