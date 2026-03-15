#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "--help" ]]; then
  cat <<USAGE
Usage: DIST_DIR=/path/to/dist commaviewd/scripts/binary-contract-check.sh [--manifest <path>]
Validates architecture, runtime deps, runpath, and bundle invariants for CommaViewD binaries.
USAGE
  exit 0
fi

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
DIST_DIR="${DIST_DIR:-$REPO_ROOT/dist}"
MANIFEST="$DIST_DIR/binary-contract.json"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --manifest) MANIFEST="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 2 ;;
  esac
done

HOST_BIN="$DIST_DIR/commaviewd-host"
ARM_BIN="$DIST_DIR/commaviewd-aarch64"
ARM_CAPNP_DEFAULT="$DIST_DIR/lib/libcapnp-0.8.0.so"
ARM_KJ_DEFAULT="$DIST_DIR/lib/libkj-0.8.0.so"
ARM_CAPNP="$ARM_CAPNP_DEFAULT"
ARM_KJ="$ARM_KJ_DEFAULT"

# Allow for newer libcapnp/libkj SONAMEs in runner environments.
if [[ ! -f "$ARM_CAPNP" ]]; then
  ARM_CAPNP="$(printf "%s\n" "$DIST_DIR/lib"/libcapnp*.so* | head -n 1 || true)"
fi
if [[ ! -f "$ARM_KJ" ]]; then
  ARM_KJ="$(printf "%s\n" "$DIST_DIR/lib"/libkj*.so* | head -n 1 || true)"
fi

for f in "$HOST_BIN" "$ARM_BIN" "$ARM_CAPNP" "$ARM_KJ"; do
  [[ -f "$f" ]] || { echo "FAIL: missing artifact $f" >&2; exit 1; }
done

host_file="$(file -b "$HOST_BIN")"
arm_file="$(file -b "$ARM_BIN")"

echo "$host_file" | grep -Eq 'x86-64|x86_64' || { echo "FAIL: host binary arch mismatch: $host_file" >&2; exit 1; }
echo "$arm_file" | grep -Eq 'aarch64|ARM aarch64' || { echo "FAIL: arm binary arch mismatch: $arm_file" >&2; exit 1; }

needed="$(aarch64-linux-gnu-readelf -d "$ARM_BIN" | awk '/NEEDED/ {gsub(/\[|\]/, "", $5); print $5}' | sort -u)"
runpath_line="$(aarch64-linux-gnu-readelf -d "$ARM_BIN" | awk '/RUNPATH|RPATH/ {print $0}')"

has_needed_prefix() {
  local prefix="$1"
  echo "$needed" | grep -Eq "^${prefix}(\.so|-[0-9])"
}

has_needed_prefix 'libcapnp' || { echo "FAIL: missing NEEDED dependency libcapnp*" >&2; echo "$needed" >&2; exit 1; }
has_needed_prefix 'libkj' || { echo "FAIL: missing NEEDED dependency libkj*" >&2; echo "$needed" >&2; exit 1; }

zmq_needed="false"
if echo "$needed" | grep -Eq '^libzmq(\.so|-[0-9])'; then
  zmq_needed="true"
fi

echo "$runpath_line" | grep -q '\$ORIGIN/lib' || { echo "FAIL: missing RUNPATH/RPATH $ORIGIN/lib" >&2; echo "$runpath_line" >&2; exit 1; }

host_size="$(stat -c '%s' "$HOST_BIN")"
arm_size="$(stat -c '%s' "$ARM_BIN")"
# Upper bounds: catch accidental static/link bloat.
[[ "$host_size" -lt 15000000 ]] || { echo "FAIL: host binary too large ($host_size bytes)" >&2; exit 1; }
[[ "$arm_size" -lt 15000000 ]] || { echo "FAIL: arm binary too large ($arm_size bytes)" >&2; exit 1; }

host_sha="$(sha256sum "$HOST_BIN" | awk '{print $1}')"
arm_sha="$(sha256sum "$ARM_BIN" | awk '{print $1}')"
capnp_sha="$(sha256sum "$ARM_CAPNP" | awk '{print $1}')"
kj_sha="$(sha256sum "$ARM_KJ" | awk '{print $1}')"

needed_json="$(echo "$needed" | awk 'BEGIN{first=1; printf("[")} {if(NF){if(!first) printf(","); printf("\"%s\"", $0); first=0}} END{printf("]")}')"

mkdir -p "$(dirname "$MANIFEST")"
cat > "$MANIFEST" <<JSON
{
  "artifacts": {
    "host": {"path": "${HOST_BIN}", "sha256": "${host_sha}", "sizeBytes": ${host_size}},
    "arm": {"path": "${ARM_BIN}", "sha256": "${arm_sha}", "sizeBytes": ${arm_size}},
    "libcapnp": {"path": "${ARM_CAPNP}", "sha256": "${capnp_sha}"},
    "libkj": {"path": "${ARM_KJ}", "sha256": "${kj_sha}"}
  },
  "runtime": {
    "needed": ${needed_json},
    "runpath": "$(echo "$runpath_line" | sed 's/"/\\"/g')",
    "zmqLinked": ${zmq_needed}
  }
}
JSON

echo "PASS: binary contract check"
echo "manifest: $MANIFEST"
