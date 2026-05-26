# Runtime Release Safety Fixes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix the release-blocking review findings between `v0.0.51-alpha` and `HEAD` before cutting the next CommaView runtime release.

**Architecture:** Keep the transformer lifecycle, but make it release-gated, test-covered, atomic, and safer during uninstall/revert. Keep support intentionally limited to official `commaai/openpilot` plus known sunnypilot remotes (`sunnypilot/sunnypilot`, `sunnypilot/openpilot`) for now, with explicit unsupported-upstream errors instead of fork fallback.

**Tech Stack:** Bash lifecycle scripts, Python transformer/templates/tests, GitHub Actions, pytest, shell contract tests.

---

### Task 1: Wire transformer pytest coverage into verification

**Files:**
- Modify: `commaviewd/scripts/run-unit-tests.sh`
- Modify: `comma4/tests/commaview_api_runtime_debug_config_test.py`

**Step 1: Update stale assertions**
- Replace old patch-era names/messages with transformer-era names/messages:
  - `patch_targets()` → `managed_targets()`
  - `backup_patch_targets()` → `backup_managed_targets()`
  - `dirty_patch_targets()` → `dirty_managed_targets()`
  - `reset_patch_targets()` → `reset_managed_targets()`
  - `force_repair_patch_targets()` → `force_repair_managed_targets()`
  - `onroad UI export patch target files...` → `onroad UI export transformer target files...`
  - restart message should mention `transformer output`.

**Step 2: Add pytest to unit runner**
- Add `python3 -m pytest "$REPO_ROOT/comma4/tests" -q` before final pass output in `commaviewd/scripts/run-unit-tests.sh`.

**Step 3: Verify**
- Run: `python3 -m pytest comma4/tests -q`
- Expected: all pass.
- Run: `commaviewd/tests/unit_tests_pipeline_test.sh`
- Expected: pass.

---

### Task 2: Add release transformer apply/verify gate

**Files:**
- Modify: `.github/workflows/commaviewd-release.yml`
- Modify if useful: `commaviewd/tests/device_test_workflow_contract_test.sh` or add a release workflow contract test if an existing pattern fits.

**Step 1: Add release gate before `Run release verification pipeline`**
- Add a workflow step equivalent to CI/device-test:
  - create `dist`
  - run `apply_onroad_ui_export_patch.sh` with `COMMAVIEWD_INSTALL_DIR=${{ github.workspace }}/comma4` and `COMMAVIEWD_OP_ROOT=${{ github.workspace }}/openpilot-src`
  - run `verify_onroad_ui_export_patch.sh --json`
  - copy `comma4/run/onroad-ui-export-status.json` to `dist/onroad-ui-export-status.json`
  - reset/clean `openpilot-src` after verification.

**Step 2: Add/extend contract coverage**
- Assert release workflow contains `apply_onroad_ui_export_patch.sh`, `verify_onroad_ui_export_patch.sh --json`, and reset/clean after verification.

**Step 3: Verify**
- Run the relevant shell contract test(s).

---

### Task 3: Pin device-test upstream checkout and upload diagnostics on failure

**Files:**
- Modify: `.github/workflows/commaviewd-device-test.yml`
- Modify: `commaviewd/tests/device_test_workflow_contract_test.sh`

**Step 1: Pin checkout**
- Change upstream checkout from `ref: ${{ matrix.target.upstream_ref }}` to `ref: ${{ steps.upstream.outputs.sha }}`.

**Step 2: Add failure diagnostics upload**
- Add an `if: failure()` upload-artifact step for partial `dist/*.json` diagnostics with `if-no-files-found: warn` and short retention.

**Step 3: Verify**
- Run: `commaviewd/tests/device_test_workflow_contract_test.sh`
- Expected: pass.

---

### Task 4: Harden uninstall/revert safety

**Files:**
- Modify: `comma4/scripts/revert_onroad_ui_export_patch.sh`
- Modify: `comma4/uninstall.sh`
- Modify: `comma4/tests/onroad_ui_export_transformer_test.py`
- Modify: `comma4/tests/commaview_api_runtime_debug_config_test.py`

**Step 1: Add offroad gating to revert**
- Mirror apply's `read_param`, `write_param`, `wait_until_offroad`, `ensure_offroad_ready`, `restore_force_offroad_mode`, and argument handling for `--force-offroad`.
- Revert should refuse while onroad unless `--force-offroad` is provided and actual offroad transition succeeds.

**Step 2: Preserve backups outside deleted install dir**
- Change revert backup root from `$INSTALL_DIR/backups/...` to `${COMMAVIEWD_BACKUP_ROOT:-/data/commaview-backups}/onroad-ui-export-revert/<timestamp>`.
- Print the backup path.
- Do not delete this path during uninstall.

**Step 3: Decide dirty semantics**
- For now, revert may reset managed targets because uninstall's purpose is cleanup, but it must preserve backup outside `/data/commaview` and require offroad.

**Step 4: Verify**
- Add pytest coverage that revert backup survives simulated uninstall deletion.
- Add contract assertions for offroad gate and external backup root.
- Run: `python3 -m pytest comma4/tests -q`.

---

### Task 5: Make transformer apply atomic enough for failures

**Files:**
- Modify: `comma4/scripts/apply_onroad_ui_export_patch.sh`
- Modify: `comma4/tests/onroad_ui_export_transformer_test.py`

**Step 1: Add rollback around transformer invocation**
- Before running `transform_onroad_ui_export.py`, backup managed targets.
- If transformer exits non-zero, reset managed targets back to git `HEAD` and restore backed-up files for tracked files/untracked helper where applicable.
- Preserve the failure backup path and print it.
- Do not hide transformer exit code.

**Step 2: Add failing-transform test**
- Create a fixture where UI state can transform but augmented road anchor fails.
- Assert apply returns non-zero and managed targets are not left partially modified.

**Step 3: Verify**
- Run focused pytest for transformer lifecycle.

---

### Task 6: Harden state parsing and JSON output

**Files:**
- Modify: `comma4/scripts/apply_onroad_ui_export_patch.sh`
- Modify: `comma4/scripts/verify_onroad_ui_export_patch.sh`
- Modify: `comma4/tests/commaview_api_runtime_debug_config_test.py`

**Step 1: Stop sourcing `STATE_ENV`**
- Replace `. "$STATE_ENV"` with a small parser that extracts exact keys using grep/sed/awk without executing shell:
  - `ONROAD_UI_EXPORT_FLAVOR`
  - `ONROAD_UI_EXPORT_OP_ROOT`

**Step 2: Validate parsed values**
- Flavor must be exactly `openpilot` or `sunnypilot`.
- OP root must match current `$OP_ROOT` before trusting flavor.

**Step 3: Emit JSON via Python**
- Replace raw `printf '{...}'` JSON construction in verify script with Python `json.dumps` using environment variables.
- Keep same fields and booleans.

**Step 4: Improve unsupported upstream error**
- If flavor cannot be detected, say official support is limited to `commaai/openpilot` and known sunnypilot remotes.

**Step 5: Verify**
- Add/adjust tests asserting no `. "$STATE_ENV"`/`source "$STATE_ENV"`, explicit unsupported message, and JSON generation path.
- Run pytest.

---

### Task 7: Restore scheduled canary coverage for supported lanes

**Files:**
- Modify: `.github/workflows/commaviewd-canary-openpilot.yml`
- Modify: `.github/workflows/commaviewd-canary-sunnypilot.yml`
- Modify/add contract tests if existing tests cover canary workflows.

**Step 1: Ensure scheduled canaries cover supported lanes**
- Keep early drift coverage.
- Add supported release/staging refs if currently absent:
  - openpilot release/staging lane as appropriate from current workflow history
  - sunnypilot staging lane.

**Step 2: Verify workflow YAML/contract tests**
- Run shell contract tests if present.
- At minimum parse workflow YAML if tooling exists.

---

### Task 8: Full verification, cleanup, commit

**Files:**
- No new generated artifacts should be committed.

**Step 1: Clean generated pycache safely**
- Use `trash` for untracked `__pycache__` directories.

**Step 2: Run verification**
- `bash -n comma4/install.sh comma4/uninstall.sh comma4/scripts/apply_onroad_ui_export_patch.sh comma4/scripts/revert_onroad_ui_export_patch.sh comma4/scripts/verify_onroad_ui_export_patch.sh`
- `python3 -m py_compile comma4/scripts/transform_onroad_ui_export.py comma4/src/commaview_export.openpilot.py comma4/src/commaview_export.sunnypilot.py`
- `python3 -m pytest comma4/tests -q`
- `commaviewd/scripts/run-unit-tests.sh`
- `commaviewd/scripts/run-verification.sh` if local upstream/toolchain context is available.

**Step 3: Review diff**
- `git diff --check`
- `git diff --stat`
- inspect changed files.

**Step 4: Commit**
- Commit on `master` with message like `fix: harden runtime transformer release lifecycle`.
