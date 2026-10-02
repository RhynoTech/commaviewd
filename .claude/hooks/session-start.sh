#!/bin/bash
# SessionStart hook for Claude Code on the web: install the arm64 cross toolchain so
# scripts/ci-local.sh (CI's checks) runs inside web sessions. Upstream sources are checked out on
# first use, under ~/.cache/commaviewd-ci-src.
set -euo pipefail

if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

REPO_ROOT="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

if command -v aarch64-linux-gnu-g++ >/dev/null 2>&1 && ls /usr/lib/aarch64-linux-gnu/libcapnp-*.so >/dev/null 2>&1; then
  exit 0
fi

# The toolchain script rewrites the apt sources (fine in a throwaway container, not on a laptop).
"$REPO_ROOT/scripts/install-commaviewd-toolchain.sh" >/tmp/commaviewd-toolchain.log 2>&1 || {
  echo "WARNING: the commaviewd toolchain didn't install (see /tmp/commaviewd-toolchain.log)."
  echo "scripts/ci-local.sh can still check patch-only targets; the verification build needs it."
  exit 0
}
echo "commaviewd session setup complete: scripts/ci-local.sh is ready"
