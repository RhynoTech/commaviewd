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
trap "rm -rf \"$TMP\"" EXIT

if [[ -f "$OP_ROOT/cereal/deprecated.capnp" ]]; then
  DEPRECATED_SCHEMA_NAME="deprecated"
elif [[ -f "$OP_ROOT/cereal/legacy.capnp" ]]; then
  DEPRECATED_SCHEMA_NAME="legacy"
else
  echo "[ERR] Missing required file: expected $OP_ROOT/cereal/deprecated.capnp or $OP_ROOT/cereal/legacy.capnp" >&2
  exit 2
fi
DEPRECATED_SCHEMA_CPP="$OP_ROOT/cereal/gen/cpp/${DEPRECATED_SCHEMA_NAME}.capnp.c++"

OP_ROOT="$OP_ROOT" "$ROOT/scripts/build-ubuntu.sh" >/dev/null

if [[ -n "${CXX:-}" ]]; then
  CXX_BIN="$CXX"
elif command -v clang++ >/dev/null 2>&1; then
  CXX_BIN="clang++"
else
  CXX_BIN="c++"
fi
INC=( -I"$ROOT/include" -I"$OP_ROOT" -I"$OP_ROOT/cereal/messaging" -I"$OP_ROOT/msgq_repo" )

"$CXX_BIN" --version >/dev/null 2>&1 || {
  echo "[ERR] C++ compiler not found: $CXX_BIN" >&2
  exit 2
}


"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_net_framing.cpp" \
  "$ROOT/src/framing.cpp" \
  -o "$TMP/test_net_framing"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_runtime_mode.cpp" \
  "$ROOT/src/mode.cpp" \
  -o "$TMP/test_runtime_mode"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_control_policy.cpp" \
  "$ROOT/src/policy.cpp" \
  "$ROOT/src/framing.cpp" \
  -o "$TMP/test_control_policy"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_telemetry_json.cpp" \
  "$ROOT/src/json_builder.cpp" \
  "$OP_ROOT/cereal/gen/cpp/log.capnp.c++" \
  "$OP_ROOT/cereal/gen/cpp/car.capnp.c++" \
  "$DEPRECATED_SCHEMA_CPP" \
  "$OP_ROOT/cereal/gen/cpp/custom.capnp.c++" \
  -lcapnp -lkj -lpthread \
  -o "$TMP/test_telemetry_json"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_telemetry_stats.cpp" \
  "$ROOT/src/telemetry_stats.cpp" \
  -o "$TMP/test_telemetry_stats"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_http_server_cloexec.cpp" \
  "$ROOT/src/http_server.cpp" \
  -o "$TMP/test_http_server_cloexec"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_ui_export_socket.cpp" \
  "$ROOT/src/ui_export_socket.cpp" \
  -o "$TMP/test_ui_export_socket"

"$TMP/test_net_framing"
"$TMP/test_runtime_mode"
"$TMP/test_control_policy"
"$TMP/test_telemetry_json"
"$TMP/test_telemetry_stats"
"$TMP/test_http_server_cloexec"
"$TMP/test_ui_export_socket"

"$ROOT/tests/control_mode_api_contract_test.sh"
"$ROOT/tests/raw_only_runtime_contract_test.sh"
echo "PASS: commaviewd unit tests passed"
