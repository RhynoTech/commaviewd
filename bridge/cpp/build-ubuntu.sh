#!/usr/bin/env bash
set -euo pipefail

# Build CommaView bridge/runtime binaries on Ubuntu Node.
# Outputs:
#   ./commaview-bridge-host      (x86_64 sanity binary, commaviewd entry with bridge compatibility)
#   ./commaview-bridge-aarch64   (comma4 deployable target)
#   ./lib/libcapnp-0.8.0.so      (deploy-side runtime dep)
#   ./lib/libkj-0.8.0.so         (deploy-side runtime dep)
#
# Usage:
#   OP_ROOT=/home/pear/openpilot-src ./build-ubuntu.sh

OP_ROOT="${OP_ROOT:-/home/pear/openpilot-src}"
OUT_DIR="${OUT_DIR:-/home/pear/CommaView/bridge/cpp}"
MAIN_SRC="${MAIN_SRC:-$OUT_DIR/commaviewd_main.cc}"
BRIDGE_SRC="${BRIDGE_SRC:-$OUT_DIR/commaview-bridge.cc}"
NET_FRAMING_SRC="$OUT_DIR/src/net/framing.cpp"
NET_SOCKET_SRC="$OUT_DIR/src/net/socket.cpp"
CONTROL_POLICY_SRC="$OUT_DIR/src/control/policy.cpp"
VIDEO_ROUTER_SRC="$OUT_DIR/src/video/router.cpp"
TELEMETRY_JSON_SRC="$OUT_DIR/src/telemetry/json_builder.cpp"
RUNTIME_MODE_SRC="$OUT_DIR/src/runtime/mode.cpp"
RUNTIME_BRIDGE_MODE_SRC="$OUT_DIR/src/runtime/bridge_mode.cpp"
RUNTIME_CONTROL_MODE_SRC="$OUT_DIR/src/runtime/control_mode.cpp"
API_HTTP_SERVER_SRC="$OUT_DIR/src/api/http_server.cpp"

HOST_OUT="$OUT_DIR/commaview-bridge-host"
ARM_OUT="$OUT_DIR/commaview-bridge-aarch64"
BUNDLE_LIB_DIR="$OUT_DIR/lib"
PATCHED_MSGQ_LOCAL="${PATCHED_MSGQ_LOCAL:-$OUT_DIR/msgq_patched.cc}"

# Runtime libs that match comma's deployed ABI for 0.8 bridge link targets.
ARM_CAPNP_SO="${ARM_CAPNP_SO:-/usr/lib/aarch64-linux-gnu/libcapnp-0.8.0.so}"
ARM_KJ_SO="${ARM_KJ_SO:-/usr/lib/aarch64-linux-gnu/libkj-0.8.0.so}"

MSGQ_SOURCE="$OP_ROOT/msgq_repo/msgq/msgq.cc"
if [[ -f "$PATCHED_MSGQ_LOCAL" ]]; then
  MSGQ_SOURCE="$PATCHED_MSGQ_LOCAL"
fi

require_file() {
  local p="$1"
  if [[ ! -f "$p" ]]; then
    echo "[ERR] Missing required file: $p" >&2
    exit 2
  fi
}

require_file "$MAIN_SRC"
require_file "$BRIDGE_SRC"
require_file "$NET_FRAMING_SRC"
require_file "$NET_SOCKET_SRC"
require_file "$TELEMETRY_JSON_SRC"
require_file "$VIDEO_ROUTER_SRC"
require_file "$CONTROL_POLICY_SRC"
require_file "$RUNTIME_MODE_SRC"
require_file "$RUNTIME_BRIDGE_MODE_SRC"
require_file "$RUNTIME_CONTROL_MODE_SRC"
require_file "$API_HTTP_SERVER_SRC"
require_file "$OP_ROOT/cereal/log.capnp"
require_file "$OP_ROOT/cereal/services.py"
require_file "$OP_ROOT/cereal/messaging/socketmaster.cc"
require_file "$OP_ROOT/msgq_repo/msgq/event.cc"
require_file "$OP_ROOT/msgq_repo/msgq/impl_fake.cc"
require_file "$OP_ROOT/msgq_repo/msgq/impl_msgq.cc"
require_file "$OP_ROOT/msgq_repo/msgq/ipc.cc"
require_file "$ARM_CAPNP_SO"
require_file "$ARM_KJ_SO"

mkdir -p "$OP_ROOT/cereal/gen/cpp"
mkdir -p "$BUNDLE_LIB_DIR"

echo "[1/5] Generating cereal headers/sources..."
echo "[info] msgq source: $MSGQ_SOURCE"
capnpc --src-prefix="$OP_ROOT/cereal" \
  "$OP_ROOT/cereal/log.capnp" \
  "$OP_ROOT/cereal/car.capnp" \
  "$OP_ROOT/cereal/legacy.capnp" \
  "$OP_ROOT/cereal/custom.capnp" \
  -o c++:"$OP_ROOT/cereal/gen/cpp/"
python3 "$OP_ROOT/cereal/services.py" > "$OP_ROOT/cereal/services.h"

COMMON_SRCS=(
  "$MAIN_SRC"
  "$BRIDGE_SRC"
  "$RUNTIME_MODE_SRC"
  "$RUNTIME_BRIDGE_MODE_SRC"
  "$RUNTIME_CONTROL_MODE_SRC"
  "$API_HTTP_SERVER_SRC"
  "$NET_FRAMING_SRC"
  "$NET_SOCKET_SRC"
  "$CONTROL_POLICY_SRC"
  "$VIDEO_ROUTER_SRC"
  "$TELEMETRY_JSON_SRC"
  "$OP_ROOT/cereal/messaging/socketmaster.cc"
  "$MSGQ_SOURCE"
  "$OP_ROOT/msgq_repo/msgq/event.cc"
  "$OP_ROOT/msgq_repo/msgq/impl_fake.cc"
  "$OP_ROOT/msgq_repo/msgq/impl_msgq.cc"
  "$OP_ROOT/msgq_repo/msgq/ipc.cc"
  "$OP_ROOT/cereal/gen/cpp/log.capnp.c++"
  "$OP_ROOT/cereal/gen/cpp/car.capnp.c++"
  "$OP_ROOT/cereal/gen/cpp/legacy.capnp.c++"
  "$OP_ROOT/cereal/gen/cpp/custom.capnp.c++"
)

echo "[2/5] Building host sanity binary (x86_64)..."
clang++ -O2 -std=c++17 -DCOMMAVIEW_BRIDGE_NO_MAIN \
  -I"$OUT_DIR/include" -I"$OP_ROOT" -I"$OP_ROOT/cereal/messaging" -I"$OP_ROOT/msgq_repo" \
  -o "$HOST_OUT" \
  "${COMMON_SRCS[@]}" \
  -lzmq -lcapnp -lkj -lpthread

echo "[3/5] Building deploy binary (aarch64)..."
aarch64-linux-gnu-g++ -O2 -std=c++17 -DCOMMAVIEW_BRIDGE_NO_MAIN \
  -I"$OUT_DIR/include" -I"$OP_ROOT" -I"$OP_ROOT/cereal/messaging" -I"$OP_ROOT/msgq_repo" \
  -L/usr/lib/aarch64-linux-gnu \
  -Wl,-rpath,'$ORIGIN/lib' \
  -o "$ARM_OUT" \
  "${COMMON_SRCS[@]}" \
  -lzmq -lcapnp -lkj -lpthread

echo "[4/5] Bundling comma runtime libs (0.8 ABI)..."
install -m 755 "$ARM_CAPNP_SO" "$BUNDLE_LIB_DIR/libcapnp-0.8.0.so"
install -m 755 "$ARM_KJ_SO" "$BUNDLE_LIB_DIR/libkj-0.8.0.so"

echo "[5/5] Done"
ls -lh "$HOST_OUT" "$ARM_OUT" "$BUNDLE_LIB_DIR/libcapnp-0.8.0.so" "$BUNDLE_LIB_DIR/libkj-0.8.0.so"
file "$HOST_OUT" "$ARM_OUT"
echo "[info] ARM dynamic deps/runpath:"
aarch64-linux-gnu-readelf -d "$ARM_OUT" | egrep 'NEEDED|RPATH|RUNPATH' || true
