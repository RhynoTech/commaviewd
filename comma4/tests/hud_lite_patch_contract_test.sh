#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OPENPILOT_PATCH="$REPO_ROOT/comma4/patches/openpilot/0001-commaview-ui-export-v2.patch"
SUNNYPILOT_PATCH="$REPO_ROOT/comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch"

fail() {
  echo "FAIL: $1" >&2
  exit 1
}

for patch in "$OPENPILOT_PATCH" "$SUNNYPILOT_PATCH"; do
  [[ -f "$patch" ]] || fail "missing patch $patch"
  grep -Fq 'commaViewControl' "$patch" || fail "$patch missing control service markers"
  grep -Fq 'commaViewScene' "$patch" || fail "$patch missing scene service markers"
  grep -Fq 'commaViewStatus' "$patch" || fail "$patch missing status service markers"
  grep -Fq 'commaview.capnp' "$patch" || fail "$patch missing dedicated commaview schema wiring"
done

echo "PASS: direct v2 UI export patch contract present"
