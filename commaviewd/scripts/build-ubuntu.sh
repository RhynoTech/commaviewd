#!/usr/bin/env bash
set -euo pipefail

# Build CommaView runtime binaries.
# Outputs (default under repo dist/):
#   dist/commaviewd-host
#   dist/commaviewd-aarch64
#   dist/lib/libcapnp-0.8.0.so
#   dist/lib/libkj-0.8.0.so

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
DEFAULT_OP_ROOT="$REPO_ROOT/../openpilot-src"
if [[ -d "$DEFAULT_OP_ROOT" ]]; then
  OP_ROOT="${OP_ROOT:-$DEFAULT_OP_ROOT}"
else
  OP_ROOT="${OP_ROOT:-$HOME/openpilot-src}"
fi
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"

MAIN_SRC="$ROOT/src/main.cc"
BRIDGE_SRC="$ROOT/src/bridge_runtime.cc"
NET_FRAMING_SRC="$ROOT/src/framing.cpp"
NET_SOCKET_SRC="$ROOT/src/socket.cpp"
CONTROL_POLICY_SRC="$ROOT/src/policy.cpp"
VIDEO_ROUTER_SRC="$ROOT/src/router.cpp"
TELEMETRY_JSON_SRC="$ROOT/src/json_builder.cpp"
TELEMETRY_STATS_SRC="$ROOT/src/telemetry_stats.cpp"
RUNTIME_MODE_SRC="$ROOT/src/mode.cpp"
RUNTIME_BRIDGE_MODE_SRC="$ROOT/src/bridge_mode.cpp"
RUNTIME_CONTROL_MODE_SRC="$ROOT/src/control_mode.cpp"
API_HTTP_SERVER_SRC="$ROOT/src/http_server.cpp"

HOST_OUT="$DIST_DIR/commaviewd-host"
ARM_OUT="$DIST_DIR/commaviewd-aarch64"
BUNDLE_LIB_DIR="$DIST_DIR/lib"
PATCHED_MSGQ_LOCAL="${PATCHED_MSGQ_LOCAL:-$ROOT/msgq_patched.cc}"

ARM_CAPNP_SO="${ARM_CAPNP_SO:-/usr/lib/aarch64-linux-gnu/libcapnp-0.8.0.so}"
ARM_KJ_SO="${ARM_KJ_SO:-/usr/lib/aarch64-linux-gnu/libkj-0.8.0.so}"
ARM_CAPNP_NAME="$(basename "$ARM_CAPNP_SO")"
ARM_KJ_NAME="$(basename "$ARM_KJ_SO")"

MSGQ_SOURCE="$OP_ROOT/msgq_repo/msgq/msgq.cc"
if [[ -f "$PATCHED_MSGQ_LOCAL" ]]; then
  MSGQ_SOURCE="$PATCHED_MSGQ_LOCAL"
fi

require_file() {
  local p="$1"
  [[ -f "$p" ]] || { echo "[ERR] Missing required file: $p" >&2; exit 2; }
}

for f in \
  "$MAIN_SRC" "$BRIDGE_SRC" "$NET_FRAMING_SRC" "$NET_SOCKET_SRC" \
  "$CONTROL_POLICY_SRC" "$VIDEO_ROUTER_SRC" "$TELEMETRY_JSON_SRC" "$TELEMETRY_STATS_SRC" \
  "$RUNTIME_MODE_SRC" "$RUNTIME_BRIDGE_MODE_SRC" "$RUNTIME_CONTROL_MODE_SRC" \
  "$API_HTTP_SERVER_SRC" \
  "$OP_ROOT/cereal/log.capnp" "$OP_ROOT/cereal/services.py" \
  "$OP_ROOT/cereal/messaging/socketmaster.cc" \
  "$OP_ROOT/msgq_repo/msgq/event.cc" "$OP_ROOT/msgq_repo/msgq/impl_fake.cc" \
  "$OP_ROOT/msgq_repo/msgq/impl_msgq.cc" "$OP_ROOT/msgq_repo/msgq/ipc.cc" \
  "$ARM_CAPNP_SO" "$ARM_KJ_SO"; do
  require_file "$f"
done

mkdir -p "$OP_ROOT/cereal/gen/cpp" "$DIST_DIR" "$BUNDLE_LIB_DIR"

echo "[1/5] Generating cereal headers/sources..."
capnpc --src-prefix="$OP_ROOT/cereal" \
  "$OP_ROOT/cereal/log.capnp" \
  "$OP_ROOT/cereal/car.capnp" \
  "$OP_ROOT/cereal/legacy.capnp" \
  "$OP_ROOT/cereal/custom.capnp" \
  -o c++:"$OP_ROOT/cereal/gen/cpp/"
python3 "$OP_ROOT/cereal/services.py" > "$OP_ROOT/cereal/services.h"

COMMON_SRCS=(
  "$MAIN_SRC" "$BRIDGE_SRC"
  "$RUNTIME_MODE_SRC" "$RUNTIME_BRIDGE_MODE_SRC" "$RUNTIME_CONTROL_MODE_SRC"
  "$API_HTTP_SERVER_SRC"
  "$NET_FRAMING_SRC" "$NET_SOCKET_SRC" "$CONTROL_POLICY_SRC"
  "$VIDEO_ROUTER_SRC" "$TELEMETRY_JSON_SRC" "$TELEMETRY_STATS_SRC"
  "$OP_ROOT/cereal/messaging/socketmaster.cc"
  "$MSGQ_SOURCE"
  "$OP_ROOT/msgq_repo/msgq/event.cc" "$OP_ROOT/msgq_repo/msgq/impl_fake.cc"
  "$OP_ROOT/msgq_repo/msgq/impl_msgq.cc" "$OP_ROOT/msgq_repo/msgq/ipc.cc"
  "$OP_ROOT/cereal/gen/cpp/log.capnp.c++" "$OP_ROOT/cereal/gen/cpp/car.capnp.c++"
  "$OP_ROOT/cereal/gen/cpp/legacy.capnp.c++" "$OP_ROOT/cereal/gen/cpp/custom.capnp.c++"
)

echo "[2/5] Building host sanity binary (x86_64)..."
clang++ -O2 -std=c++17 -DCOMMAVIEW_BRIDGE_NO_MAIN \
  -I"$ROOT/include" -I"$OP_ROOT" -I"$OP_ROOT/cereal/messaging" -I"$OP_ROOT/msgq_repo" \
  -o "$HOST_OUT" "${COMMON_SRCS[@]}" \
  -lzmq -lcapnp -lkj -lpthread

echo "[3/5] Building deploy binary (aarch64)..."
aarch64-linux-gnu-g++ -O2 -std=c++17 -DCOMMAVIEW_BRIDGE_NO_MAIN \
  -I"$ROOT/include" -I"$OP_ROOT" -I"$OP_ROOT/cereal/messaging" -I"$OP_ROOT/msgq_repo" \
  -L/usr/lib/aarch64-linux-gnu -Wl,-rpath,\$ORIGIN/lib \
  -o "$ARM_OUT" "${COMMON_SRCS[@]}" \
  -lzmq -lcapnp -lkj -lpthread

echo "[4/5] Bundling comma runtime libs (detected ABI)..."
install -m 755 "$ARM_CAPNP_SO" "$BUNDLE_LIB_DIR/$ARM_CAPNP_NAME"
install -m 755 "$ARM_KJ_SO" "$BUNDLE_LIB_DIR/$ARM_KJ_NAME"

echo "[5/5] Done"
ls -lh "$HOST_OUT" "$ARM_OUT" "$BUNDLE_LIB_DIR/$ARM_CAPNP_NAME" "$BUNDLE_LIB_DIR/$ARM_KJ_NAME"
file "$HOST_OUT" "$ARM_OUT"
aarch64-linux-gnu-readelf -d "$ARM_OUT" | egrep "NEEDED|RPATH|RUNPATH" || true
