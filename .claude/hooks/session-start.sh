#!/bin/bash
# SessionStart hook for Claude Code on the web: install the commaviewd C++
# toolchain and the upstream openpilot source so the full unit-test suite
# (commaviewd/scripts/run-unit-tests.sh) can run inside web sessions.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

REPO_ROOT="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
OP_ROOT="${OP_ROOT:-$(cd "$REPO_ROOT/.." && pwd)/openpilot-src}"
UPSTREAM_REPO="${COMMAVIEWD_UPSTREAM_REPO:-https://github.com/commaai/openpilot.git}"
UPSTREAM_REF="${COMMAVIEWD_UPSTREAM_REF:-release-mici-staging}"

# Container images may ship apt sources (PPAs) that the session network policy
# blocks; park them so `apt-get update` succeeds against the Ubuntu mirrors.
if ls /etc/apt/sources.list.d/* >/dev/null 2>&1; then
  sudo mkdir -p /etc/apt/disabled-sources
  for f in /etc/apt/sources.list.d/*; do
    [ -f "$f" ] && sudo mv "$f" /etc/apt/disabled-sources/
  done
fi

# Idempotent: the toolchain script re-runs cheaply once packages are cached,
# but skip it entirely when the key tools are already present.
if ! command -v capnp >/dev/null 2>&1 \
    || ! command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 \
    || ! ls /usr/lib/aarch64-linux-gnu/libcapnp-*.so >/dev/null 2>&1; then
  "$REPO_ROOT/scripts/install-commaviewd-toolchain.sh"
fi

if [ ! -d "$OP_ROOT/cereal" ]; then
  git clone --depth 1 --branch "$UPSTREAM_REF" --recurse-submodules --shallow-submodules \
    "$UPSTREAM_REPO" "$OP_ROOT"
fi

if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
  echo "export OP_ROOT=\"$OP_ROOT\"" >> "$CLAUDE_ENV_FILE"
fi

echo "commaviewd session setup complete (OP_ROOT=$OP_ROOT)"
