#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

[[ -f "$REPO_ROOT/comma4/patches/openpilot/0001-hud-lite-export.patch" ]] || fail "missing openpilot HUD-lite patch"
[[ -f "$REPO_ROOT/comma4/patches/sunnypilot/0001-hud-lite-export.patch" ]] || fail "missing sunnypilot HUD-lite patch"
grep -Fqi "hud-lite" "$REPO_ROOT/comma4/install.sh" || fail "install path should manage HUD-lite patch lifecycle"
grep -Fqi "hud-lite" "$REPO_ROOT/comma4/upgrade.sh" || fail "upgrade path should manage HUD-lite patch lifecycle"
grep -Eqi "hud[-_ ]lite|hudLite" "$REPO_ROOT/commaviewd/src/control_mode.cpp" || fail "control mode should expose HUD-lite health or repair status"

echo "PASS: HUD-lite patch lifecycle contract present"
