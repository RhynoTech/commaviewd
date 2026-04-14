#!/usr/bin/env bash
set -euo pipefail

# Build CommaView runtime binaries.
# Outputs (default under repo dist/):
#   dist/commaviewd-host
#   dist/commaviewd-aarch64
#   dist/lib/libcapnp-*.so
#   dist/lib/libkj-*.so

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
DEFAULT_OP_ROOT="$REPO_ROOT/../openpilot-src"
if [[ -d "$DEFAULT_OP_ROOT" ]]; then
  OP_ROOT="${OP_ROOT:-$DEFAULT_OP_ROOT}"
else
  OP_ROOT="${OP_ROOT:-$HOME/openpilot-src}"
fi
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
HOST_CXX="${HOST_CXX:-${CXX:-c++}}"
CROSS_CXX="${CROSS_CXX:-aarch64-linux-gnu-g++}"
SKIP_ARM="${COMMAVIEWD_SKIP_ARM:-0}"

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
UI_EXPORT_SOCKET_SRC="$ROOT/src/ui_export_socket.cpp"

HOST_OUT="$DIST_DIR/commaviewd-host"
ARM_OUT="$DIST_DIR/commaviewd-aarch64"
BUNDLE_LIB_DIR="$DIST_DIR/lib"
PATCHED_MSGQ_LOCAL="${PATCHED_MSGQ_LOCAL:-$ROOT/msgq_patched.cc}"

detect_arm_lib() {
  local pattern="$1"
  local first_match=""
  first_match="$(compgen -G "$pattern" | head -n 1 || true)"
  if [[ -n "$first_match" ]]; then
    printf '%s\n' "$first_match"
    return 0
  fi
  return 1
}

ARM_CAPNP_SO="${ARM_CAPNP_SO:-$(detect_arm_lib /usr/lib/aarch64-linux-gnu/libcapnp-*.so || true)}"
ARM_KJ_SO="${ARM_KJ_SO:-$(detect_arm_lib /usr/lib/aarch64-linux-gnu/libkj-*.so || true)}"
ARM_CAPNP_NAME="$(basename "$ARM_CAPNP_SO")"
ARM_KJ_NAME="$(basename "$ARM_KJ_SO")"

MSGQ_SOURCE="$OP_ROOT/msgq_repo/msgq/msgq.cc"
if [[ -f "$PATCHED_MSGQ_LOCAL" ]]; then
  MSGQ_SOURCE="$PATCHED_MSGQ_LOCAL"
fi

DEPRECATED_SCHEMA_NAME=""
if [[ -f "$OP_ROOT/cereal/deprecated.capnp" ]]; then
  DEPRECATED_SCHEMA_NAME="deprecated"
elif [[ -f "$OP_ROOT/cereal/legacy.capnp" ]]; then
  DEPRECATED_SCHEMA_NAME="legacy"
else
  echo "[ERR] Missing required file: expected $OP_ROOT/cereal/deprecated.capnp or $OP_ROOT/cereal/legacy.capnp" >&2
  exit 2
fi
DEPRECATED_SCHEMA_PATH="$OP_ROOT/cereal/${DEPRECATED_SCHEMA_NAME}.capnp"
DEPRECATED_SCHEMA_CPP="$OP_ROOT/cereal/gen/cpp/${DEPRECATED_SCHEMA_NAME}.capnp.c++"

require_file() {
  local p="$1"
  [[ -f "$p" ]] || { echo "[ERR] Missing required file: $p" >&2; exit 2; }
}

for f in \
  "$MAIN_SRC" "$BRIDGE_SRC" "$NET_FRAMING_SRC" "$NET_SOCKET_SRC" \
  "$CONTROL_POLICY_SRC" "$VIDEO_ROUTER_SRC" "$TELEMETRY_JSON_SRC" "$TELEMETRY_STATS_SRC" \
  "$RUNTIME_MODE_SRC" "$RUNTIME_BRIDGE_MODE_SRC" "$RUNTIME_CONTROL_MODE_SRC" \
  "$API_HTTP_SERVER_SRC" "$UI_EXPORT_SOCKET_SRC" \
  "$OP_ROOT/cereal/log.capnp" "$OP_ROOT/cereal/services.py" \
  "$OP_ROOT/cereal/messaging/socketmaster.cc" \
  "$OP_ROOT/msgq_repo/msgq/event.cc" "$OP_ROOT/msgq_repo/msgq/impl_fake.cc" \
  "$OP_ROOT/msgq_repo/msgq/impl_msgq.cc" "$OP_ROOT/msgq_repo/msgq/ipc.cc"; do
  require_file "$f"
done

if [[ "$SKIP_ARM" != "1" ]]; then
  for f in "$ARM_CAPNP_SO" "$ARM_KJ_SO"; do
    require_file "$f"
  done
fi

mkdir -p "$OP_ROOT/cereal/gen/cpp" "$DIST_DIR" "$BUNDLE_LIB_DIR"

echo "[1/5] Generating cereal headers/sources..."
capnpc --src-prefix="$OP_ROOT/cereal" \
  "$OP_ROOT/cereal/log.capnp" \
  "$OP_ROOT/cereal/car.capnp" \
  "$DEPRECATED_SCHEMA_PATH" \
  "$OP_ROOT/cereal/custom.capnp" \
  -o c++:"$OP_ROOT/cereal/gen/cpp/"
python3 "$OP_ROOT/cereal/services.py" > "$OP_ROOT/cereal/services.h"

COMMON_SRCS=(
  "$MAIN_SRC" "$BRIDGE_SRC"
  "$RUNTIME_MODE_SRC" "$RUNTIME_BRIDGE_MODE_SRC" "$RUNTIME_CONTROL_MODE_SRC"
  "$API_HTTP_SERVER_SRC" "$UI_EXPORT_SOCKET_SRC"
  "$NET_FRAMING_SRC" "$NET_SOCKET_SRC" "$CONTROL_POLICY_SRC"
  "$VIDEO_ROUTER_SRC" "$TELEMETRY_JSON_SRC" "$TELEMETRY_STATS_SRC"
  "$OP_ROOT/cereal/messaging/socketmaster.cc"
  "$MSGQ_SOURCE"
  "$OP_ROOT/msgq_repo/msgq/event.cc" "$OP_ROOT/msgq_repo/msgq/impl_fake.cc"
  "$OP_ROOT/msgq_repo/msgq/impl_msgq.cc" "$OP_ROOT/msgq_repo/msgq/ipc.cc"
  "$OP_ROOT/cereal/gen/cpp/log.capnp.c++" "$OP_ROOT/cereal/gen/cpp/car.capnp.c++"
  "$DEPRECATED_SCHEMA_CPP" "$OP_ROOT/cereal/gen/cpp/custom.capnp.c++"
)

echo "[2/5] Building host sanity binary (x86_64)..."
"$HOST_CXX" -O2 -std=c++17 -DCOMMAVIEW_BRIDGE_NO_MAIN \
  -I"$ROOT/include" -I"$OP_ROOT" -I"$OP_ROOT/cereal/messaging" -I"$OP_ROOT/msgq_repo" \
  -o "$HOST_OUT" "${COMMON_SRCS[@]}" \
  -lzmq -lcapnp -lkj -lpthread

if [[ "$SKIP_ARM" == "1" ]]; then
  echo "[3/5] Skipping aarch64 build (COMMAVIEWD_SKIP_ARM=1)"
  echo "[4/5] Skipping runtime lib bundle (COMMAVIEWD_SKIP_ARM=1)"
  echo "[5/5] Done"
  ls -lh "$HOST_OUT"
  file "$HOST_OUT"
  exit 0
fi

echo "[3/5] Building deploy binary (aarch64)..."
"$CROSS_CXX" -O2 -std=c++17 -DCOMMAVIEW_BRIDGE_NO_MAIN \
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
