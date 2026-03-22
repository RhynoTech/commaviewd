# Schema Drift Prune Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove the obsolete broad Android schema drift gate from `commaviewd`, rewire CI/canaries to check HUD-lite patch reality instead, and leave `CommaView` with only Android-owned snapshot/codegen guards plus a short prune note.

**Architecture:** `commaviewd` will stop carrying any `android-schema` manifests or schema-diff scripts. Workflow enforcement will move to a HUD-lite patch applicability/status step against the currently checked-out upstream tree, with summaries/artifacts pointing at HUD-lite status JSON. `CommaView` stays unchanged functionally; it only gets a note documenting that runtime-side schema drift checks were intentionally pruned.

**Tech Stack:** Bash, GitHub Actions YAML, Python removal, repo docs under `docs/plans/`

---

### Task 1: Lock the new workflow contract with a failing test

**Files:**
- Create: `commaviewd/tests/hud_lite_ci_contract_test.sh`
- Modify: `commaviewd/tests/unit_tests_pipeline_test.sh`
- Test: `commaviewd/tests/hud_lite_ci_contract_test.sh`

**Step 1: Write the failing test**

Create `commaviewd/tests/hud_lite_ci_contract_test.sh` to assert:
- `commaviewd-ci.yml`, `commaviewd-canary-openpilot.yml`, and `commaviewd-canary-sunnypilot.yml` do **not** mention `android-schema`, `check-android-schema-drift`, or `dist/android-schema-drift.json`
- all three workflows **do** mention HUD-lite patch validation (`apply_hud_lite_patch.sh` or `verify_hud_lite_patch.sh`) and surface a HUD-lite status artifact/summary

Also wire that new test into `commaviewd/tests/unit_tests_pipeline_test.sh`.

**Step 2: Run test to verify it fails**

Run: `cd /home/pear/commaviewd && commaviewd/tests/hud_lite_ci_contract_test.sh`
Expected: FAIL because current workflows still mention Android schema drift.

### Task 2: Prune the old schema drift gate and rewire workflows

**Files:**
- Delete: `android-schema/manifest.json`
- Delete: `android-schema/contract-manifest.json`
- Delete: `android-schema/ignore-manifest.json`
- Delete: `scripts/check-android-schema-drift.sh`
- Delete: `scripts/check_android_schema_drift.py`
- Delete: `commaviewd/tests/schema_contract_manifest_test.sh`
- Delete: `commaviewd/tests/schema_contract_drift_test.py`
- Delete: `commaviewd/tests/fixtures/schema_contract/**`
- Modify: `.github/workflows/commaviewd-ci.yml`
- Modify: `.github/workflows/commaviewd-canary-openpilot.yml`
- Modify: `.github/workflows/commaviewd-canary-sunnypilot.yml`
- Modify: `commaviewd/tests/unit_tests_pipeline_test.sh`
- Test: `commaviewd/tests/hud_lite_ci_contract_test.sh`

**Step 1: Re-run the failing workflow contract test**

Run: `cd /home/pear/commaviewd && commaviewd/tests/hud_lite_ci_contract_test.sh`
Expected: FAIL on Android schema drift references.

**Step 2: Make the minimal implementation changes**

- Delete the old schema drift manifests/scripts/tests/fixtures.
- In `commaviewd-ci.yml`:
  - remove schema-drift trigger paths
  - add `comma4/**` (and any other runtime guard paths needed) so HUD-lite changes actually trigger CI
  - replace the Android schema drift step with a HUD-lite patch applicability/verify step against `${{ github.workspace }}/openpilot-src`
  - copy `comma4/run/hud-lite-status.json` into `dist/hud-lite-status.json` for summary/artifact upload
  - remove `dist/android-schema-drift.json` summary/upload references and replace them with `dist/hud-lite-status.json`
- In both canary workflows:
  - replace schema drift step with the same HUD-lite applicability/status step against the matrix checkout
  - replace schema drift summary/upload references with HUD-lite status references

**Step 3: Run test to verify it passes**

Run: `cd /home/pear/commaviewd && commaviewd/tests/hud_lite_ci_contract_test.sh`
Expected: PASS.

### Task 3: Add short prune notes in both repos

**Files:**
- Create: `docs/plans/2026-03-22-schema-drift-prune-note.md` (in `commaviewd`)
- Create: `/home/pear/CommaView/docs/plans/2026-03-22-runtime-schema-drift-prune-note.md`

**Step 1: Write the notes**

Document plainly that:
- `commaviewd` no longer carries a broad Android schema drift gate
- runtime CI/canaries now validate HUD-lite patch applicability/health instead
- `CommaView` remains responsible for schema snapshot/codegen integrity only

### Task 4: Verify the remaining guards

**Files:**
- Test: `commaviewd/tests/hud_lite_ci_contract_test.sh`
- Test: `commaviewd/tests/unit_tests_pipeline_test.sh`
- Test: `comma4/tests/hud_lite_patch_contract_test.sh`
- Test: `comma4/tests/hud_lite_canary_applicability_test.sh`
- Test: `commaviewd/scripts/run-verification.sh`
- Test: `/home/pear/CommaView/scripts/verify-schema-snapshot.sh`
- Test: `/home/pear/CommaView/scripts/verify-no-committed-generated-bindings.sh`
- Test: `/home/pear/CommaView/scripts/verify-generated-bindings-reproducible.sh`

**Step 1: Run runtime-side verification**

Run:
```bash
cd /home/pear/commaviewd
commaviewd/tests/hud_lite_ci_contract_test.sh
commaviewd/tests/unit_tests_pipeline_test.sh
comma4/tests/hud_lite_patch_contract_test.sh
comma4/tests/hud_lite_canary_applicability_test.sh
OP_ROOT=/home/pear/openpilot-src commaviewd/scripts/run-verification.sh
```
Expected: PASS, with HUD-lite checks green and no schema drift gate involved.

**Step 2: Run Android-owned guards**

Run:
```bash
cd /home/pear/CommaView
./scripts/verify-schema-snapshot.sh
./scripts/verify-no-committed-generated-bindings.sh
./scripts/verify-generated-bindings-reproducible.sh
```
Expected: PASS.
