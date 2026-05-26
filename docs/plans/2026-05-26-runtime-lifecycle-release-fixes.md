# Runtime Lifecycle Release Fixes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix the three release-blocking runtime lifecycle issues found in review: installer rollback must restore upstream UI changes, uninstall must not delete recovery tools when revert is unavailable, and release checksums must be portable after download.

**Architecture:** Keep the existing shell-script lifecycle model. Add behavior tests first, then minimally extend installer cleanup, uninstall safety, and bundle checksum generation. Avoid changing runtime C++ behavior.

**Tech Stack:** Bash lifecycle scripts, Python pytest contract tests, GitHub Actions release workflow contract tests.

---

### Task 1: Installer rollback restores upstream UI tree after late install failure

**Files:**
- Modify: `comma4/install.sh`
- Test: `comma4/tests/onroad_ui_export_transformer_test.py`

**Step 1: Write the failing test**

Add a pytest that builds a fake install directory, fake staged bundle, fake upstream repo, then simulates: transformer apply succeeds, a later install step fails, and cleanup runs. The test should assert upstream managed UI files match their original git state after rollback.

**Step 2: Run test to verify it fails**

Run:

```bash
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py::test_install_cleanup_restores_upstream_targets_after_late_failure -q
```

Expected: FAIL because installer cleanup currently restores `/data/commaview` only, not the already-mutated upstream UI tree.

**Step 3: Write minimal implementation**

In `comma4/install.sh`:
- Track whether transformer apply succeeded.
- During cleanup, if install did not complete and transformer apply succeeded, call the installed revert helper before restoring the previous install tree.
- Preserve the existing backup behavior if revert fails.
- Do not restart the old runtime until the upstream tree has been reverted.

**Step 4: Run test to verify it passes**

Run the same pytest target and expect PASS.

**Step 5: Commit checkpoint**

```bash
git add comma4/install.sh comma4/tests/onroad_ui_export_transformer_test.py
git commit -m "fix: restore upstream patch on failed install"
```

### Task 2: Uninstall aborts when recovery helper is missing

**Files:**
- Modify: `comma4/uninstall.sh`
- Test: `comma4/tests/commaview_api_runtime_debug_config_test.py`

**Step 1: Write the failing test**

Add a pytest that creates a fake install tree without an executable revert helper, runs uninstall, and asserts:
- non-zero exit
- install tree still exists
- stop/remove steps did not run
- output explains that uninstall preserved recovery state

**Step 2: Run test to verify it fails**

Run:

```bash
python3 -m pytest comma4/tests/commaview_api_runtime_debug_config_test.py::test_uninstall_script_aborts_when_revert_helper_is_missing -q
```

Expected: FAIL because uninstall currently warns and removes the install tree.

**Step 3: Write minimal implementation**

In `comma4/uninstall.sh`:
- If the revert helper is missing or non-executable, print an error and exit before stopping services, removing boot hook, or deleting files.
- Keep the existing behavior when the helper exists and succeeds.

**Step 4: Run test to verify it passes**

Run the same pytest target and expect PASS.

**Step 5: Commit checkpoint**

```bash
git add comma4/uninstall.sh comma4/tests/commaview_api_runtime_debug_config_test.py
git commit -m "fix: abort unsafe uninstall without revert helper"
```

### Task 3: Release checksum file is portable

**Files:**
- Modify: `tools/release/comma4-build-bundle.sh`
- Modify: `.github/workflows/commaviewd-release.yml`
- Test: `commaviewd/tests/release_workflow_contract_test.sh`

**Step 1: Write the failing test**

Update the release workflow contract test to require:
- checksum generation uses the asset basename from inside the release directory
- checksum validation changes into the release directory before running `sha256sum -c`

**Step 2: Run test to verify it fails**

Run:

```bash
commaviewd/tests/release_workflow_contract_test.sh
```

Expected: FAIL because current checksum paths are absolute/runner-local.

**Step 3: Write minimal implementation**

In `tools/release/comma4-build-bundle.sh`:

```bash
(
  cd "$OUT_DIR"
  sha256sum "${NAME}.tar.gz" > "${NAME}.tar.gz.sha256"
)
```

In the release workflow, validate with:

```bash
(
  cd "release/${TAG}"
  sha256sum -c "commaview-comma4-${TAG}.tar.gz.sha256"
)
```

**Step 4: Run test to verify it passes**

Run the release workflow contract test and expect PASS.

**Step 5: Commit checkpoint**

```bash
git add tools/release/comma4-build-bundle.sh .github/workflows/commaviewd-release.yml commaviewd/tests/release_workflow_contract_test.sh
git commit -m "fix: publish portable runtime checksums"
```

### Task 4: Full verification and final commit/status

**Files:**
- Verify all touched files.

**Step 1: Run focused tests**

```bash
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py comma4/tests/commaview_api_runtime_debug_config_test.py -q
commaviewd/tests/release_workflow_contract_test.sh
commaviewd/tests/device_test_workflow_contract_test.sh
commaviewd/tests/ci_workflow_contract_test.sh
```

**Step 2: Clean generated artifacts**

Use `gio trash` or configured trash command for generated `dist/`, `release/`, and `__pycache__/` artifacts if created.

**Step 3: Final repo status**

```bash
pwd
git remote -v
git branch --show-current
git status --short --branch
```

Expected: repo on `master`, correct runtime remote, clean except expected ahead commits.
