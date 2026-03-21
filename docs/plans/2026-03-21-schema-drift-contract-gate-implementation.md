# Schema Drift Contract Gate Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the current file-hash Android schema drift check with a fail-closed, field/enum-level contract gate in `commaviewd`, using a handwritten checked-in contract manifest plus a reviewed ignore manifest, and enforce it separately against openpilot and sunnypilot.

**Architecture:** Keep the current shell entrypoint and workflow wiring stable, but move the comparison logic into a Python helper that parses upstream Cap'n Proto schema into a normalized graph, diffs it against `android-schema/contract-manifest.json`, subtracts reviewed ignores from `android-schema/ignore-manifest.json`, and emits machine-readable reports plus concise CI summaries. Preserve `android-schema/manifest.json` for coarse provenance, but make the new contract manifests the authoritative enforcement layer.

**Tech Stack:** Bash entrypoint scripts, Python 3 helper/test code, JSON manifests, GitHub Actions workflows, existing `commaviewd` test and verification scripts, upstream openpilot/sunnypilot checkouts.

---

### Task 1: Add fixture-backed failing tests for the structured schema diff engine

**Files:**
- Create: `commaviewd/tests/schema_contract_drift_test.py`
- Create: `commaviewd/tests/fixtures/schema_contract/base/cereal/log.capnp`
- Create: `commaviewd/tests/fixtures/schema_contract/add_field/cereal/log.capnp`
- Create: `commaviewd/tests/fixtures/schema_contract/remove_field/cereal/log.capnp`
- Create: `commaviewd/tests/fixtures/schema_contract/type_change/cereal/log.capnp`
- Create: `commaviewd/tests/fixtures/schema_contract/enum_add/cereal/car.capnp`
- Create: `commaviewd/tests/fixtures/schema_contract/ignore_case/cereal/custom.capnp`
- Test target: future Python helper under `scripts/`

**Step 1: Write the failing test**

```python
from pathlib import Path

from scripts.check_android_schema_drift import diff_contract, load_contract, load_ignores, parse_schema_tree

FIXTURES = Path(__file__).parent / "fixtures" / "schema_contract"

def test_additive_field_is_reported_when_unignored():
    contract = load_contract({
        "services": {
            "DeviceState": {
                "file": "cereal/log.capnp",
                "fields": {
                    "started": {"ordinal": 0, "type": "Bool"}
                },
                "enums": {}
            }
        }
    })
    ignores = load_ignores({"ignores": []})
    upstream = parse_schema_tree(FIXTURES / "add_field")

    report = diff_contract(contract, upstream, ignores, label="fixture")

    assert report["unignoredCount"] == 1
    assert report["items"][0]["driftClass"] == "field-added"
```

**Step 2: Run test to verify it fails**

Run: `cd /home/pear/commaviewd && python3 -m pytest commaviewd/tests/schema_contract_drift_test.py -q`

Expected: `FAIL` because the import target/helper functions do not exist yet.

**Step 3: Write minimal implementation scaffolding**
- Add an importable Python module entrypoint under `scripts/`.
- Add stubs for `load_contract`, `load_ignores`, `parse_schema_tree`, and `diff_contract` that return deterministic placeholder structures.
- Do **not** implement the full parser yet; only make the test importable and ready for the next red/green loops.

**Step 4: Run test to verify it still fails for the real reason**

Run: `cd /home/pear/commaviewd && python3 -m pytest commaviewd/tests/schema_contract_drift_test.py -q`

Expected: `FAIL` on assertion mismatch instead of import/module-not-found.

**Step 5: Commit**

```bash
cd /home/pear/commaviewd
git add commaviewd/tests/schema_contract_drift_test.py commaviewd/tests/fixtures/schema_contract scripts/check_android_schema_drift.py
git commit -m "test(schema): add contract drift diff fixtures"
```

### Task 2: Add checked-in contract and ignore manifests with contract tests

**Files:**
- Create: `android-schema/contract-manifest.json`
- Create: `android-schema/ignore-manifest.json`
- Create: `commaviewd/tests/schema_contract_manifest_test.sh`
- Modify: `commaviewd/tests/unit_tests_pipeline_test.sh`

**Step 1: Write the failing manifest contract test**

```bash
#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
for file in \
  "$ROOT/android-schema/contract-manifest.json" \
  "$ROOT/android-schema/ignore-manifest.json"; do
  [[ -f "$file" ]] || { echo "FAIL: missing $file"; exit 1; }
done
python3 - <<'PY' "$ROOT/android-schema/contract-manifest.json" "$ROOT/android-schema/ignore-manifest.json"
import json, pathlib, sys
contract = json.loads(pathlib.Path(sys.argv[1]).read_text())
ignore = json.loads(pathlib.Path(sys.argv[2]).read_text())
assert contract["version"] == 1
assert contract["services"]
assert ignore["version"] == 1
assert "ignores" in ignore
PY
```

**Step 2: Run test to verify it fails**

Run: `cd /home/pear/commaviewd && commaviewd/tests/schema_contract_manifest_test.sh`

Expected: `FAIL: missing .../android-schema/contract-manifest.json`

**Step 3: Write minimal manifests**
- Create `android-schema/contract-manifest.json` with metadata plus one tiny reviewed bootstrap service entry.
- Create `android-schema/ignore-manifest.json` with versioned empty `ignores` list.
- Update `commaviewd/tests/unit_tests_pipeline_test.sh` to require the new manifest test script and its `--help`/basic execution path if you add one.

Example contract seed:

```json
{
  "version": 1,
  "notes": "Handwritten schema contract for commaviewd telemetry drift enforcement.",
  "services": {
    "DeviceState": {
      "file": "cereal/log.capnp",
      "fields": {
        "started": { "ordinal": 0, "type": "Bool" }
      },
      "enums": {}
    }
  }
}
```

**Step 4: Run tests to verify they pass**

Run: `cd /home/pear/commaviewd && commaviewd/tests/schema_contract_manifest_test.sh`

Expected: `PASS` or zero-exit validation.

**Step 5: Commit**

```bash
cd /home/pear/commaviewd
git add android-schema/contract-manifest.json android-schema/ignore-manifest.json commaviewd/tests/schema_contract_manifest_test.sh commaviewd/tests/unit_tests_pipeline_test.sh
git commit -m "test(schema): add checked-in contract and ignore manifests"
```

### Task 3: Implement the normalized schema parser and fail-closed diff logic

**Files:**
- Create: `scripts/check_android_schema_drift.py`
- Modify: `scripts/check-android-schema-drift.sh`
- Modify: `android-schema/contract-manifest.json`
- Modify: `android-schema/ignore-manifest.json`
- Test: `commaviewd/tests/schema_contract_drift_test.py`

**Step 1: Extend tests with the real failure matrix**
- Add cases for `field-removed`, `field-type-changed`, `enum-value-added`, `service-added`, and `ignored` drift.
- Add one test proving source text reordering does **not** change normalized keys.

Example assertion shape:

```python
assert {(item["service"], item["symbol"], item["driftClass"]) for item in report["items"]} == {
    ("DeviceState", "thermalStatus", "enum-value-added"),
    ("CarState", "vEgo", "field-type-changed"),
}
assert report["unignoredCount"] == 2
```

**Step 2: Run test to verify it fails**

Run: `cd /home/pear/commaviewd && python3 -m pytest commaviewd/tests/schema_contract_drift_test.py -q`

Expected: `FAIL` because the parser/diff engine still returns placeholders.

**Step 3: Write minimal implementation**
- In `scripts/check_android_schema_drift.py`, implement:
  - manifest loading/validation
  - Cap'n Proto source scanning from `--upstream-root`
  - normalized graph extraction for services, fields, ordinals, types, and enums
  - ignore filtering by upstream scope + service + symbol + drift class
  - JSON report serialization
- In `scripts/check-android-schema-drift.sh`, replace the inline hash-only Python with a wrapper that invokes the new Python helper while preserving CLI compatibility.
- Grow `android-schema/contract-manifest.json` from the one-service seed to the reviewed **all-services** handwritten baseline.
- Keep `android-schema/manifest.json` intact for provenance until a later cleanup deliberately removes it.

Suggested wrapper shape:

```bash
python3 scripts/check_android_schema_drift.py \
  --contract android-schema/contract-manifest.json \
  --ignore-manifest android-schema/ignore-manifest.json \
  --upstream-root "$UPSTREAM_ROOT" \
  --label "$LABEL" \
  --mode "$MODE"
```

**Step 4: Run tests to verify they pass**

Run: `cd /home/pear/commaviewd && python3 -m pytest commaviewd/tests/schema_contract_drift_test.py -q`

Expected: all fixture cases `PASS`.

**Step 5: Commit**

```bash
cd /home/pear/commaviewd
git add scripts/check_android_schema_drift.py scripts/check-android-schema-drift.sh android-schema/contract-manifest.json android-schema/ignore-manifest.json commaviewd/tests/schema_contract_drift_test.py
git commit -m "feat(schema): enforce field-level telemetry drift contract"
```

### Task 4: Add report/suggest modes and wire enforced checks into both upstream workflows

**Files:**
- Modify: `scripts/check_android_schema_drift.py`
- Modify: `.github/workflows/commaviewd-ci.yml`
- Modify: `.github/workflows/commaviewd-canary-openpilot.yml`
- Modify: `.github/workflows/commaviewd-canary-sunnypilot.yml`
- Test: local script invocation against both upstream trees

**Step 1: Write the failing behavior test / smoke assertions**
- Add a smoke test block to `commaviewd/tests/schema_contract_manifest_test.sh` or a new shell test that asserts:
  - `--mode report` exits zero and writes a report
  - `--mode suggest` exits zero and prints candidate contract/ignore snippets
  - `--mode fail` exits non-zero when fixture drift is unignored

Example shell assertion:

```bash
python3 scripts/check_android_schema_drift.py --help | grep -q -- "--mode"
python3 scripts/check_android_schema_drift.py \
  --contract android-schema/contract-manifest.json \
  --ignore-manifest android-schema/ignore-manifest.json \
  --upstream-root commaviewd/tests/fixtures/schema_contract/add_field \
  --label fixture \
  --mode fail && exit 1 || true
```

**Step 2: Run test to verify it fails**

Run: `cd /home/pear/commaviewd && commaviewd/tests/schema_contract_manifest_test.sh`

Expected: `FAIL` because report/suggest/help behavior is incomplete.

**Step 3: Write minimal implementation and workflow wiring**
- Add `report` and `suggest` modes to the Python helper.
- Make `suggest` print candidate JSON blocks without editing checked-in files.
- Flip workflow invocations from `--mode warn` to `--mode fail` once manifests are stable.
- Keep separate labels and artifacts for `openpilot` and `sunnypilot` so fork-specific drift stays visible.

Workflow run lines should look like:

```yaml
- name: Android schema drift check
  run: |
    set -euxo pipefail
    ./scripts/check-android-schema-drift.sh \
      --upstream-root "${{ github.workspace }}/openpilot-src" \
      --label "${{ matrix.target.name }}" \
      --mode fail
```

**Step 4: Run tests and local smoke checks to verify they pass**

Run:
- `cd /home/pear/commaviewd && commaviewd/tests/schema_contract_manifest_test.sh`
- `cd /home/pear/commaviewd && ./scripts/check-android-schema-drift.sh --upstream-root /home/pear/openpilot-src --label openpilot --mode report`
- `cd /home/pear/commaviewd && ./scripts/check-android-schema-drift.sh --upstream-root /home/pear/sunnypilot-src --label sunnypilot --mode report`

Expected:
- tests pass
- report JSON written under `dist/`
- no unignored drift for the reviewed manifests, or precise actionable output if there is still cleanup to do before flipping CI

**Step 5: Commit**

```bash
cd /home/pear/commaviewd
git add scripts/check_android_schema_drift.py .github/workflows/commaviewd-ci.yml .github/workflows/commaviewd-canary-openpilot.yml .github/workflows/commaviewd-canary-sunnypilot.yml commaviewd/tests/schema_contract_manifest_test.sh
git commit -m "ci(schema): enforce reviewed telemetry drift contract"
```

### Task 5: Run full verification before claiming success

**Files:**
- Modify: none unless verification exposes real defects
- Verify: current repo tree only

**Step 1: Run targeted verification**

Run:
- `cd /home/pear/commaviewd && python3 -m pytest commaviewd/tests/schema_contract_drift_test.py -q`
- `cd /home/pear/commaviewd && commaviewd/tests/schema_contract_manifest_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/unit_tests_pipeline_test.sh`

Expected: all pass.

**Step 2: Run broader existing verification**

Run:
- `cd /home/pear/commaviewd && OP_ROOT=/home/pear/openpilot-src commaviewd/scripts/run-unit-tests.sh`
- `cd /home/pear/commaviewd && ./scripts/check-android-schema-drift.sh --upstream-root /home/pear/openpilot-src --label openpilot --mode fail`
- `cd /home/pear/commaviewd && ./scripts/check-android-schema-drift.sh --upstream-root /home/pear/sunnypilot-src --label sunnypilot --mode fail`

Expected:
- unit tests pass
- both drift checks pass or produce precise remaining contract/ignore work

**Step 3: Confirm clean repo state**

Run: `cd /home/pear/commaviewd && git status --short --branch`

Expected: clean `master` or only the intentional contract-gate changes.

**Step 4: Push only after verification is clean**

```bash
cd /home/pear/commaviewd
git push origin master
```

**Step 5: Post-merge confidence check**

Run: `cd /home/pear/commaviewd && gh run list --limit 10`

Expected: `commaviewd-ci`, `commaviewd-canary-openpilot`, and `commaviewd-canary-sunnypilot` show the enforced drift gate passing on the new commits.
