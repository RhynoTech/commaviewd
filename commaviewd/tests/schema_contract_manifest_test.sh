#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
for file in \
  "$ROOT/android-schema/contract-manifest.json" \
  "$ROOT/android-schema/ignore-manifest.json"; do
  [[ -f "$file" ]] || { echo "FAIL: missing $file"; exit 1; }
done
python3 - "$ROOT/android-schema/contract-manifest.json" "$ROOT/android-schema/ignore-manifest.json" <<'PY'
import json, pathlib, sys
contract = json.loads(pathlib.Path(sys.argv[1]).read_text())
ignore = json.loads(pathlib.Path(sys.argv[2]).read_text())
assert contract["version"] == 1
assert contract["services"]
assert ignore["version"] == 1
assert "ignores" in ignore
PY

echo "PASS: schema contract manifests exist and validate"
