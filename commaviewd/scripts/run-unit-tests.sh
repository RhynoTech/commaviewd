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
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
export COMMAVIEWD_HOST_BIN="$DIST_DIR/commaviewd-host"

if [[ -f "$OP_ROOT/cereal/log.capnp" ]]; then
  OP_SOURCE_ROOT="$OP_ROOT"
elif [[ -f "$OP_ROOT/openpilot/cereal/log.capnp" ]]; then
  OP_SOURCE_ROOT="$OP_ROOT/openpilot"
else
  echo "[ERR] Missing upstream cereal tree below $OP_ROOT" >&2
  exit 2
fi

if [[ -f "$OP_SOURCE_ROOT/cereal/deprecated.capnp" ]]; then
  DEPRECATED_SCHEMA_NAME="deprecated"
elif [[ -f "$OP_SOURCE_ROOT/cereal/legacy.capnp" ]]; then
  DEPRECATED_SCHEMA_NAME="legacy"
else
  echo "[ERR] Missing required deprecated or legacy cereal schema below $OP_SOURCE_ROOT" >&2
  exit 2
fi
DEPRECATED_SCHEMA_CPP="$OP_SOURCE_ROOT/cereal/gen/cpp/${DEPRECATED_SCHEMA_NAME}.capnp.c++"

OP_ROOT="$OP_ROOT" "$ROOT/scripts/build-ubuntu.sh" >/dev/null

if [[ -n "${CXX:-}" ]]; then
  CXX_BIN="$CXX"
elif command -v clang++ >/dev/null 2>&1; then
  CXX_BIN="clang++"
else
  CXX_BIN="c++"
fi
INC=( -I"$ROOT/include" -I"$OP_ROOT" -I"$OP_SOURCE_ROOT" -I"$OP_SOURCE_ROOT/cereal/gen/cpp" -I"$OP_SOURCE_ROOT/cereal/messaging" -I"$OP_ROOT/msgq_repo" )

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

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" \
  "$ROOT/tests/test_telemetry_policy.cpp" \
  -o "$TMP/test_telemetry_policy"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" \
  "$ROOT/tests/test_video_transport_policy.cpp" \
  "$ROOT/src/video_transport_policy.cpp" \
  -o "$TMP/test_video_transport_policy"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" -DCOMMAVIEW_VIDEO_CHUNK_PROTOCOL_TESTING \
  "$ROOT/tests/test_video_chunk_protocol.cpp" \
  "$ROOT/src/video_chunk_protocol.cpp" \
  -o "$TMP/test_video_chunk_protocol"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" -I"$ROOT/src" \
  "$ROOT/tests/test_video_send_accounting.cpp" \
  "$ROOT/src/runtime_video_send_accounting.cpp" \
  "$ROOT/src/framing.cpp" \
  -o "$TMP/test_video_send_accounting"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_http_server_cloexec.cpp" \
  "$ROOT/src/http_server.cpp" \
  -o "$TMP/test_http_server_cloexec"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_ui_export_socket.cpp" \
  "$ROOT/src/ui_export_socket.cpp" \
  -o "$TMP/test_ui_export_socket"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_source_recording_archive.cpp" \
  "$ROOT/src/source_recording_archive.cpp" \
  -o "$TMP/test_source_recording_archive"

"$CXX_BIN" -O2 -std=c++17 "${INC[@]}" \
  "$ROOT/tests/test_msgq_header_recovery.cpp" \
  "$DIST_DIR/msgq-commaviewd.cc" \
  -lpthread -o "$TMP/test_msgq_header_recovery"

"$TMP/test_net_framing"
"$TMP/test_runtime_mode"
"$TMP/test_control_policy"
"$TMP/test_telemetry_policy"
"$TMP/test_video_transport_policy"
"$TMP/test_video_chunk_protocol"
"$TMP/test_video_send_accounting"
"$TMP/test_http_server_cloexec"
"$TMP/test_ui_export_socket"
"$TMP/test_source_recording_archive"
"$TMP/test_msgq_header_recovery"

"$ROOT/tests/control_mode_api_contract_test.sh"
"$ROOT/tests/control_mode_pairing_integration_test.sh"
python3 "$ROOT/tests/source_archive_endpoint_integration_test.py"
"$ROOT/tests/local_discovery_contract_test.sh"
"$ROOT/tests/upstream_interface_guard_transformer_test.sh"
"$ROOT/tests/device_test_workflow_contract_test.sh"
"$ROOT/tests/ci_workflow_contract_test.sh"
"$ROOT/tests/release_workflow_contract_test.sh"
"$ROOT/tests/raw_only_runtime_contract_test.sh"
"$ROOT/tests/video_msgq_conflate_contract_test.sh"
"$ROOT/tests/video_transport_bridge_contract_test.sh"
"$ROOT/tests/runtime_split_transport_contract_test.sh"
"$ROOT/tests/build_video_schema_contract_test.sh"
python3 -m pytest "$REPO_ROOT/comma/tests" -q
echo "PASS: commaviewd unit tests passed"
