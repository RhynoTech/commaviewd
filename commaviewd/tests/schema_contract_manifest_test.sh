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

python3 "$ROOT/scripts/check_android_schema_drift.py" --help | grep -q -- "--mode"
if python3 "$ROOT/scripts/check_android_schema_drift.py" \
  --contract "$ROOT/android-schema/contract-manifest.json" \
  --ignore-manifest "$ROOT/android-schema/ignore-manifest.json" \
  --upstream-root "$ROOT/commaviewd/tests/fixtures/schema_contract/add_field" \
  --label fixture \
  --mode fail >/tmp/schema-contract-fail.out 2>&1; then
  echo "FAIL: expected --mode fail to exit non-zero on fixture drift"
  cat /tmp/schema-contract-fail.out
  exit 1
fi
python3 "$ROOT/scripts/check_android_schema_drift.py" \
  --contract "$ROOT/android-schema/contract-manifest.json" \
  --ignore-manifest "$ROOT/android-schema/ignore-manifest.json" \
  --upstream-root "$ROOT/commaviewd/tests/fixtures/schema_contract/add_field" \
  --label fixture \
  --mode report >/tmp/schema-contract-report.out
[[ -f "$ROOT/dist/android-schema-drift.json" ]] || { echo "FAIL: missing dist/android-schema-drift.json"; exit 1; }
python3 "$ROOT/scripts/check_android_schema_drift.py" \
  --contract "$ROOT/android-schema/contract-manifest.json" \
  --ignore-manifest "$ROOT/android-schema/ignore-manifest.json" \
  --upstream-root "$ROOT/commaviewd/tests/fixtures/schema_contract/add_field" \
  --label fixture \
  --mode suggest >/tmp/schema-contract-suggest.out
grep -q "Candidate contract manifest" /tmp/schema-contract-suggest.out

echo "PASS: schema contract manifests and CLI modes validate"
