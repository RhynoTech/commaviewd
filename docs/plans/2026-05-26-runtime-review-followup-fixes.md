# Runtime Review Follow-up Fixes Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Fix every release-blocking issue found in the `v0.0.51-alpha..HEAD` code review before pushing or cutting the next CommaView runtime release.

**Architecture:** Keep the transformer-based lifecycle, but make verification behavior-based, make installer/uninstaller failure paths recoverable, and make release publication/promotion explicit. Treat GitHub Release assets and Firebase current-release updates as separate operations: publishing an immutable tag is not the same as promoting it live.

**Tech Stack:** Bash lifecycle scripts, Python pytest lifecycle tests, GitHub Actions, GitHub CLI release publishing, Firebase current-release manifest updater, shell contract tests.

---

## Review findings covered

1. Existing GitHub Releases can be overwritten on tag-push/rerun because release upload uses `--clobber` without a push-event overwrite gate.
2. Firebase current-release pointer can move backward because every valid release tag updates Firebase unconditionally.
3. Release provenance manifests are generated but not uploaded as release assets.
4. `pytest` is required by `run-unit-tests.sh` but not installed explicitly by the CI toolchain.
5. Installer rollback does not back up/restore/clean the new `comma4/src` install tree.
6. `--force-repair` backups can collide because backup directories use second-resolution timestamps.
7. `uninstall.sh` stops services/removes boot hook before revert can reject unsafe onroad mutation.
8. `verify_onroad_ui_export_patch.sh` can false-positive if one augmented road path is transformed and the other existing path is stale.
9. `onroad_ui_export_patch_contract_test.sh` is stale/red because it greps old hand-built JSON implementation details.
10. Adjacent CI race: `commaviewd-ci.yml` resolves upstream SHA but checks out the moving ref on cache miss.

---

### Task 1: Fix red/stale contracts and make verify check every augmented road path

**Files:**
- Modify: `comma4/scripts/verify_onroad_ui_export_patch.sh`
- Modify: `comma4/tests/onroad_ui_export_transformer_test.py`
- Modify: `comma4/tests/onroad_ui_export_patch_contract_test.sh`
- Modify if needed: `commaviewd/tests/unit_tests_pipeline_test.sh`

**Step 1: Write a failing regression test for stale second augmented path**

Add to `comma4/tests/onroad_ui_export_transformer_test.py`:

```python
def test_verify_fails_when_any_existing_augmented_path_is_stale(tmp_path):
    op_root = write_full_augmented_tree(tmp_path)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)

    applied = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")
    assert applied.returncode == 0, applied.stderr

    subprocess.run(
        ["git", "checkout", "--", "selfdrive/ui/onroad/augmented_road_view.py"],
        cwd=op_root,
        check=True,
    )

    result = run_lifecycle_script(
        REPO_ROOT / "comma4" / "scripts" / "verify_onroad_ui_export_patch.sh",
        install_dir,
        op_root,
        "--json",
    )

    assert result.returncode == 1
    payload = json.loads(result.stdout)
    assert payload["patchVerified"] is False
    assert payload["repairNeeded"] is True
```

Also import `json` at the top if not already present.

**Step 2: Run the failing test**

Run:

```bash
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q -k stale
```

Expected: FAIL because verify currently returns `patchVerified=true` when only one augmented path is stale.

**Step 3: Update verify script to check all existing augmented paths**

In `verify_onroad_ui_export_patch.sh`:

- Keep `AUGMENTED_ROAD_PATHS` as the candidate list.
- Build an `existing_augmented_paths` array containing every existing candidate file.
- If no candidate exists, use the first path as the missing expected path and fail verification.
- Replace the single `$AUGMENTED_ROAD_PATH` checks with a loop that requires camera relay and projection markers for every existing path.

Implementation shape:

```bash
existing_augmented_paths=()
for candidate in "${AUGMENTED_ROAD_PATHS[@]}"; do
  if [ -f "$candidate" ]; then
    existing_augmented_paths+=("$candidate")
  fi
done

augmented_paths_present=false
if [ "${#existing_augmented_paths[@]}" -gt 0 ]; then
  augmented_paths_present=true
fi

onroad_camera_relay_present=true
onroad_projection_present=true
if [ "${#existing_augmented_paths[@]}" -eq 0 ]; then
  onroad_camera_relay_present=false
  onroad_projection_present=false
else
  for augmented_path in "${existing_augmented_paths[@]}"; do
    if ! check_augmented_camera_relay "$augmented_path"; then
      onroad_camera_relay_present=false
    fi
    if ! check_augmented_projection "$augmented_path"; then
      onroad_projection_present=false
    fi
  done
fi
```

Add small helper functions for readability:

```bash
check_augmented_camera_relay() {
  local path="$1"
  check_fixed 'self._update_commaview_camera_export()' "$path" && \
    check_fixed 'def _update_commaview_camera_export(self):' "$path" && \
    check_fixed 'active_camera="wideRoad" if self.stream_type == WIDE_CAM else "road"' "$path"
}

check_augmented_projection() {
  local path="$1"
  check_fixed 'model_transform = video_transform @ calib_transform' "$path" && \
    check_fixed 'exporter.set_onroad_projection(' "$path" && \
    check_fixed 'active_camera="wideRoad" if is_wide_camera else "road"' "$path" && \
    check_fixed 'video_frame_matrix=self._cached_matrix' "$path" && \
    { check_fixed 'camera_offset=getattr(self._model_renderer, "_camera_offset", 0.0)' "$path" || \
      check_fixed 'camera_offset=getattr(self.model_renderer, "_camera_offset", 0.0)' "$path"; }
}
```

Keep the helper-side projection marker check against `$HELPER_PATH` separate.

**Step 4: Replace stale implementation-grep contract with behavior/JSON contract**

In `comma4/tests/onroad_ui_export_patch_contract_test.sh`, remove:

```bash
grep -Fq '"method":"%s"' "$VERIFY_SCRIPT" || fail "verify status missing method field"
```

Replace it with contract checks that verify current behavior without coupling to `printf` implementation:

```bash
grep -Fq '"method": os.environ.get("METHOD", "")' "$VERIFY_SCRIPT" || fail "verify JSON should include method field"
grep -Fq 'json.dumps(payload' "$VERIFY_SCRIPT" || fail "verify status should be emitted via json.dumps"
grep -Fq 'METHOD="$method"' "$VERIFY_SCRIPT" || fail "verify status should pass method to JSON payload"
```

If this still feels too implementation-coupled during execution, prefer a temp-tree behavior test in this shell script or move the assertion into pytest and make this shell contract only verify script wiring.

**Step 5: Verify Task 1**

Run:

```bash
comma4/tests/onroad_ui_export_patch_contract_test.sh
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q
commaviewd/tests/unit_tests_pipeline_test.sh
```

Expected:
- Contract test passes.
- Transformer tests pass.
- Pipeline script contract passes.

**Step 6: Commit Task 1**

```bash
git add comma4/scripts/verify_onroad_ui_export_patch.sh \
  comma4/tests/onroad_ui_export_transformer_test.py \
  comma4/tests/onroad_ui_export_patch_contract_test.sh \
  commaviewd/tests/unit_tests_pipeline_test.sh
git commit -m "fix: verify all transformer augmented paths"
```

---

### Task 2: Harden installer rollback and backup uniqueness

**Files:**
- Modify: `comma4/install.sh`
- Modify: `comma4/scripts/apply_onroad_ui_export_patch.sh`
- Modify: `comma4/tests/onroad_ui_export_transformer_test.py`
- Modify: `comma4/tests/commaview_api_runtime_debug_config_test.py`

**Step 1: Write failing tests for unique backup directories**

Add to `comma4/tests/onroad_ui_export_transformer_test.py`:

```python
def test_apply_script_force_repair_and_transform_failure_use_distinct_backups(tmp_path):
    broken_augmented = AUGMENTED_ROAD_VIEW.replace(
        "    self._model_renderer.set_transform(video_transform @ calib_transform)\n",
        "",
    )
    op_root = write_augmented_tree(tmp_path, broken_augmented)
    init_git_repo(op_root)
    install_dir = prepare_lifecycle_install_dir(tmp_path, op_root)
    ui_state = op_root / "selfdrive" / "ui" / "ui_state.py"
    ui_state.write_text(ui_state.read_text() + "\n# dirty user change\n")

    result = run_lifecycle_script(APPLY_SCRIPT, install_dir, op_root, "--force-repair")

    assert result.returncode != 0
    backup_lines = [line for line in result.stderr.splitlines() if "backups written to" in line or "restored managed targets from" in line]
    backup_paths = {line.rsplit(" ", 1)[-1] for line in backup_lines}
    assert len(backup_paths) >= 2
```

**Step 2: Run the failing focused test**

Run:

```bash
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q -k distinct_backups
```

Expected: FAIL if timestamp collision causes identical backup paths or output cannot prove distinct paths.

**Step 3: Implement unique backup directories**

In `apply_onroad_ui_export_patch.sh`, replace timestamp-only backup root creation:

```bash
local backup_root="$INSTALL_DIR/backups/onroad-ui-export/$(date -u +%Y%m%d-%H%M%S)"
mkdir -p "$backup_root"
```

with a unique directory:

```bash
local backup_parent="$INSTALL_DIR/backups/onroad-ui-export"
local backup_root=""
mkdir -p "$backup_parent"
backup_root="$(mktemp -d "$backup_parent/$(date -u +%Y%m%d-%H%M%S).XXXXXX")"
```

Keep printing the exact backup path.

**Step 4: Add rollback coverage for `src` tree**

Add contract assertions in `comma4/tests/commaview_api_runtime_debug_config_test.py` that `install.sh` includes `src` in:

- `backup_managed_install_tree`
- `clean_managed_install_tree`
- `restore_previous_install_tree`

If a behavior-level shell test is practical, create one instead: simulate an existing install with old `src`, run install with a forced failure after `deploy_required_scripts`, and assert old `src` restored. If not practical, keep the string contract plus manual inspection.

**Step 5: Implement `src` rollback management**

In `comma4/install.sh`:

- Add `src` to the `backup_managed_install_tree` loop.
- Add `$INSTALL_DIR/src` to `clean_managed_install_tree` removal.
- Add `$INSTALL_DIR/src` to `restore_previous_install_tree` removal before restore.
- Recreate `$INSTALL_DIR/src` in the mkdir section if needed before `deploy_required_scripts`.

Expected managed install tree includes:

```bash
commaviewd
VERSION
start.sh
stop.sh
uninstall.sh
runtime-debug.defaults.json
version.env
lib
scripts
src
patches
```

**Step 6: Verify Task 2**

Run:

```bash
bash -n comma4/install.sh comma4/scripts/apply_onroad_ui_export_patch.sh
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q -k 'distinct_backups or force_repair or rollback'
python3 -m pytest comma4/tests/commaview_api_runtime_debug_config_test.py -q
```

Expected: all pass.

**Step 7: Commit Task 2**

```bash
git add comma4/install.sh \
  comma4/scripts/apply_onroad_ui_export_patch.sh \
  comma4/tests/onroad_ui_export_transformer_test.py \
  comma4/tests/commaview_api_runtime_debug_config_test.py
git commit -m "fix: harden installer rollback backups"
```

---

### Task 3: Make uninstall onroad rejection atomic enough

**Files:**
- Modify: `comma4/uninstall.sh`
- Modify: `comma4/scripts/revert_onroad_ui_export_patch.sh`
- Modify: `comma4/tests/onroad_ui_export_transformer_test.py`
- Modify: `comma4/tests/commaview_api_runtime_debug_config_test.py`

**Step 1: Write failing test for uninstall onroad preflight ordering**

Add a contract test in `comma4/tests/commaview_api_runtime_debug_config_test.py`:

```python
def test_uninstall_preflights_revert_before_stopping_services_and_hook_removal():
    text = (REPO_ROOT / "comma4" / "uninstall.sh").read_text()
    preflight_index = text.index("--preflight-only")
    stop_index = text.index("Stopping services")
    hook_index = text.index("Removing boot hook")
    assert preflight_index < stop_index
    assert preflight_index < hook_index
```

If the current test module does not expose `REPO_ROOT`, use its existing pattern.

**Step 2: Add `--preflight-only` to revert helper**

In `revert_onroad_ui_export_patch.sh`:

- Add `PREFLIGHT_ONLY=0`.
- Parse `--preflight-only`.
- After `ensure_offroad_ready`, if `PREFLIGHT_ONLY=1`, exit 0 before backups/reset/restart.

Usage string:

```bash
Usage: revert_onroad_ui_export_patch.sh [--force-offroad] [--preflight-only]
```

**Step 3: Call revert preflight before uninstall mutation**

In `uninstall.sh`, before stopping services or removing boot hook:

```bash
revert_args=()
if [ "$FORCE_OFFROAD" = "1" ]; then
  revert_args+=(--force-offroad)
fi

if [ -x "$INSTALL_DIR/scripts/revert_onroad_ui_export_patch.sh" ]; then
  COMMAVIEWD_INSTALL_DIR="$INSTALL_DIR" bash "$INSTALL_DIR/scripts/revert_onroad_ui_export_patch.sh" "${revert_args[@]}" --preflight-only
  preflight_ec=$?
  if [ "$preflight_ec" -ne 0 ]; then
    echo "ERROR: direct v2 onroad UI export transformer revert preflight failed with exit $preflight_ec; uninstall aborted before stopping services or removing boot hook" >&2
    exit "$preflight_ec"
  fi
fi
```

Then reuse `revert_args` for the actual revert later.

**Step 4: Verify Task 3**

Run:

```bash
bash -n comma4/uninstall.sh comma4/scripts/revert_onroad_ui_export_patch.sh
python3 -m pytest comma4/tests/commaview_api_runtime_debug_config_test.py -q -k uninstall
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q -k revert
```

Expected: all pass.

**Step 5: Commit Task 3**

```bash
git add comma4/uninstall.sh \
  comma4/scripts/revert_onroad_ui_export_patch.sh \
  comma4/tests/onroad_ui_export_transformer_test.py \
  comma4/tests/commaview_api_runtime_debug_config_test.py
git commit -m "fix: preflight runtime uninstall safety"
```

---

### Task 4: Lock down GitHub Release overwrite and Firebase promotion

**Files:**
- Modify: `.github/workflows/commaviewd-release.yml`
- Modify: `commaviewd/tests/release_workflow_contract_test.sh`
- Optional create: `scripts/compare-runtime-release-tags.mjs` if monotonic compare is implemented outside workflow shell.

**Step 1: Write failing release workflow contract checks**

Extend `commaviewd/tests/release_workflow_contract_test.sh` to assert:

```bash
assert_contains "Release $TAG already exists; overwrite requires manual dispatch with allow_overwrite=true" "$WORKFLOW" "release workflow should block all existing release overwrites by default"
assert_contains "promote_current" "$WORKFLOW" "release workflow should require explicit current-release promotion input"
assert_contains "if: github.event_name == 'workflow_dispatch' && inputs.promote_current == true" "$WORKFLOW" "Firebase current release update should be manual promotion only"
```

Run:

```bash
commaviewd/tests/release_workflow_contract_test.sh
```

Expected: FAIL before workflow changes.

**Step 2: Add manual promotion input**

In `.github/workflows/commaviewd-release.yml`, under `workflow_dispatch.inputs`, add:

```yaml
promote_current:
  description: 'Promote this runtime tag to Firebase current-release after publishing assets'
  required: false
  default: false
  type: boolean
```

**Step 3: Block all existing release overwrites unless explicitly allowed manually**

Replace the existing release-exists guard with:

```bash
if [[ "$release_exists" = "1" ]]; then
  if [[ "${{ github.event_name }}" != "workflow_dispatch" || "${{ inputs.allow_overwrite }}" != "true" ]]; then
    echo "Release $TAG already exists; overwrite requires manual dispatch with allow_overwrite=true" >&2
    exit 1
  fi
fi
```

Keep `--clobber` only after this guard.

**Step 4: Gate Firebase current-release update**

Add an `if:` condition to the Firebase update step:

```yaml
if: github.event_name == 'workflow_dispatch' && inputs.promote_current == true
```

Rename the step to make intent explicit:

```yaml
- name: Promote Firebase current runtime release
```

**Step 5: Add monotonic guard if practical**

If `scripts/update-firebase-current-release.mjs` can cheaply read the current runtime tag, add a `--require-newer-runtime-tag` option there and use it in the workflow.

If not practical in this pass, document that `promote_current` is the explicit human gate and leave monotonic comparison for a follow-up hardening task.

**Step 6: Verify Task 4**

Run:

```bash
commaviewd/tests/release_workflow_contract_test.sh
python3 - <<'PY'
import yaml, pathlib
for p in ['.github/workflows/commaviewd-release.yml']:
    yaml.safe_load(pathlib.Path(p).read_text())
print('workflow yaml parsed')
PY
```

If PyYAML is unavailable locally, use the repo's existing YAML parse pattern or Ruby/Python stdlib fallback if present.

Expected: contract passes and workflow parses.

**Step 7: Commit Task 4**

```bash
git add .github/workflows/commaviewd-release.yml commaviewd/tests/release_workflow_contract_test.sh scripts/update-firebase-current-release.mjs
git commit -m "ci: require explicit runtime release promotion"
```

Omit `scripts/update-firebase-current-release.mjs` from `git add` if not changed.

---

### Task 5: Publish release provenance and pin CI upstream checkout

**Files:**
- Modify: `.github/workflows/commaviewd-release.yml`
- Modify: `.github/workflows/commaviewd-ci.yml`
- Modify: `commaviewd/tests/release_workflow_contract_test.sh`
- Modify/add: relevant CI workflow contract test if present

**Step 1: Add failing release provenance contract checks**

Extend `commaviewd/tests/release_workflow_contract_test.sh`:

```bash
assert_contains "dist/reproducible-build-manifest.json" "$WORKFLOW" "release should publish reproducible build manifest"
assert_contains "dist/upstream-interface-manifest.json" "$WORKFLOW" "release should publish upstream interface manifest"
assert_contains "dist/binary-contract.json" "$WORKFLOW" "release should publish binary contract manifest"
assert_contains "dist/release-smoke-manifest.json" "$WORKFLOW" "release should publish release smoke manifest"
assert_contains "dist/onroad-ui-export-status.json" "$WORKFLOW" "release should publish transformer status manifest"
```

Run:

```bash
commaviewd/tests/release_workflow_contract_test.sh
```

Expected: FAIL until upload list includes manifests.

**Step 2: Upload provenance manifests as GitHub Release assets**

In release workflow, define manifest paths:

```bash
PROVENANCE_ASSETS=(
  dist/reproducible-build-manifest.json
  dist/upstream-interface-manifest.json
  dist/binary-contract.json
  dist/release-smoke-manifest.json
  dist/onroad-ui-export-status.json
)
```

Before upload, assert they exist:

```bash
for asset in "${PROVENANCE_ASSETS[@]}"; do
  [[ -f "$asset" ]] || { echo "Missing provenance asset: $asset" >&2; exit 1; }
done
```

Update upload:

```bash
gh release upload "$TAG" "$ASSET_TGZ" "$ASSET_SHA" "${PROVENANCE_ASSETS[@]}" \
  --repo "$GITHUB_REPOSITORY" \
  --clobber
```

**Step 3: Pin adjacent CI upstream checkout to resolved SHA**

In `.github/workflows/commaviewd-ci.yml`, find the upstream checkout step that uses:

```yaml
ref: ${{ matrix.target.upstream_ref }}
```

Change to:

```yaml
ref: ${{ steps.upstream.outputs.sha }}
```

Only do this where the workflow already has a `Resolve upstream SHA` step for that checkout.

**Step 4: Add/extend CI workflow contract coverage**

If there is an existing CI contract test, add:

```bash
assert_contains 'ref: ${{ steps.upstream.outputs.sha }}' "$CI_WORKFLOW" "commaviewd CI upstream checkout should be pinned to resolved SHA"
```

If no contract exists, add this assertion to the closest workflow contract test or create `commaviewd/tests/ci_workflow_contract_test.sh`, then wire it into `run-unit-tests.sh` and `unit_tests_pipeline_test.sh`.

**Step 5: Verify Task 5**

Run:

```bash
commaviewd/tests/release_workflow_contract_test.sh
commaviewd/scripts/run-unit-tests.sh
```

Expected: all pass.

**Step 6: Commit Task 5**

```bash
git add .github/workflows/commaviewd-release.yml \
  .github/workflows/commaviewd-ci.yml \
  commaviewd/tests/release_workflow_contract_test.sh \
  commaviewd/tests/ci_workflow_contract_test.sh \
  commaviewd/scripts/run-unit-tests.sh \
  commaviewd/tests/unit_tests_pipeline_test.sh
git commit -m "ci: publish runtime release provenance"
```

Only add files that exist/changed.

---

### Task 6: Make pytest dependency deterministic

**Files:**
- Modify: `scripts/install-commaviewd-toolchain.sh`
- Modify: relevant toolchain/CI contract test if present
- Optional modify: `.github/workflows/commaviewd-release.yml`, `.github/workflows/commaviewd-device-test.yml`, `.github/workflows/commaviewd-ci.yml` only if dependency install belongs in workflow instead of toolchain script.

**Step 1: Write/update contract for pytest dependency**

Add a contract assertion where toolchain dependencies are tested:

```bash
grep -Fq 'python3-pytest' scripts/install-commaviewd-toolchain.sh || { echo "FAIL: toolchain install should include python3-pytest"; exit 1; }
```

If no appropriate contract exists, add this to `commaviewd/tests/unit_tests_pipeline_test.sh`.

**Step 2: Run the failing contract**

Run:

```bash
commaviewd/tests/unit_tests_pipeline_test.sh
```

Expected: FAIL until `python3-pytest` is installed.

**Step 3: Install pytest through apt**

In `scripts/install-commaviewd-toolchain.sh`, add `python3-pytest` to the apt package list:

```bash
python3 \
python3-pytest \
libzmq3-dev \
```

Prefer apt over pip/venv here because this script is already apt-based and used by CI runners.

**Step 4: Verify Task 6**

Run:

```bash
commaviewd/tests/unit_tests_pipeline_test.sh
```

Expected: pass.

**Step 5: Commit Task 6**

```bash
git add scripts/install-commaviewd-toolchain.sh commaviewd/tests/unit_tests_pipeline_test.sh
git commit -m "ci: install pytest for runtime checks"
```

---

### Task 7: Full local verification and cleanup

**Files:**
- No generated outputs committed.

**Step 1: Run syntax checks**

```bash
bash -n comma4/install.sh \
  comma4/uninstall.sh \
  comma4/scripts/apply_onroad_ui_export_patch.sh \
  comma4/scripts/revert_onroad_ui_export_patch.sh \
  comma4/scripts/verify_onroad_ui_export_patch.sh
```

Expected: no output, exit 0.

**Step 2: Run Python compile checks**

```bash
python3 -m py_compile \
  comma4/scripts/transform_onroad_ui_export.py \
  comma4/src/commaview_export.openpilot.py \
  comma4/src/commaview_export.sunnypilot.py
```

Expected: no output, exit 0.

**Step 3: Run pytest suite**

```bash
python3 -m pytest comma4/tests -q
```

Expected: all pass.

**Step 4: Run commaviewd unit pipeline**

```bash
commaviewd/scripts/run-unit-tests.sh
```

Expected: `PASS: commaviewd unit tests passed`.

**Step 5: Run full verification pipeline if local upstream/toolchain context is present**

```bash
commaviewd/scripts/run-verification.sh
```

Expected: `PASS: commaviewd verification pipeline complete`.

**Step 6: Check diff and whitespace**

```bash
git diff --check
git diff --stat v0.0.51-alpha..HEAD
git status --short --branch
```

Expected:
- `git diff --check` clean.
- Generated `dist/`, `release/`, and `__pycache__/` are not staged or committed.

**Step 7: Clean generated artifacts safely**

Use `gio trash` if available:

```bash
for p in comma4/scripts/__pycache__ comma4/src/__pycache__ comma4/tests/__pycache__ dist release; do
  [ -e "$p" ] && gio trash "$p"
done
```

Expected: no generated artifacts remain in `git status --short`.

---

### Task 8: Device validation after fixes

**Files:**
- No source changes expected unless validation finds a bug.

**Step 1: Build local device package**

Use the final local HEAD short SHA:

```bash
HEAD_SHORT="$(git rev-parse --short HEAD)"
tools/release/comma4-build-bundle.sh "device-local-${HEAD_SHORT}"
tar -C comma4 -czf "/tmp/commaview-comma4-companions-${HEAD_SHORT}.tar.gz" \
  install.sh start.sh stop.sh uninstall.sh runtime-debug.defaults.json scripts src
```

Expected: release tarball and `.sha256` are created under `release/device-local-${HEAD_SHORT}/`.

**Step 2: Install on off-car comma if available**

Target: `192.168.68.104`, SSH identity `~/.ssh/node-keys/id_ed25519_comma4`.

Preflight:

```bash
ssh -i ~/.ssh/node-keys/id_ed25519_comma4 -o IdentitiesOnly=yes comma@192.168.68.104 \
  'echo IsOnroad=$(cat /data/params/d/IsOnroad 2>/dev/null || true); git -C /data/openpilot remote -v | head -2; git -C /data/openpilot rev-parse HEAD'
```

Expected: `IsOnroad=0` before mutation.

Install via file-backed env, same pattern as previous validation.

**Step 3: Verify device install**

Run on device:

```bash
cat /data/commaview/version.env
/data/commaview/scripts/verify_onroad_ui_export_patch.sh --json
(ss -ltnp 2>/dev/null || netstat -ltnp 2>/dev/null || true) | grep -E ':(5002|8200|8201|8202)'
pgrep -af commaviewd
```

Expected:
- `patchVerified=true`
- `repairNeeded=false`
- `method=transformer`
- correct flavor for device remote
- `serviceMarkerCount=19`
- bridge/control listening on `8200/8201/8202` and `5002`.

**Step 4: Verify repair path**

On device, reset managed openpilot targets and remove helper, then verify failure and repair success:

```bash
cd /data/openpilot
for rel in selfdrive/ui/commaview_export.py selfdrive/ui/ui_state.py selfdrive/ui/mici/onroad/augmented_road_view.py selfdrive/ui/onroad/augmented_road_view.py; do
  git reset -q HEAD -- "$rel" >/dev/null 2>&1 || true
  if git ls-files --error-unmatch -- "$rel" >/dev/null 2>&1; then
    git checkout -- "$rel"
  else
    rm -f "$rel"
  fi
done
set +e
/data/commaview/scripts/verify_onroad_ui_export_patch.sh --json
verify_ec=$?
set -e
test "$verify_ec" = "1"
/data/commaview/scripts/apply_onroad_ui_export_patch.sh --force-repair
/data/commaview/scripts/verify_onroad_ui_export_patch.sh --json
```

Expected: first verify fails with `repairNeeded=true`; repair restores clean verification.

**Step 5: Verify uninstall and final reinstall**

Run packaged uninstall, assert clean removal and external backup preservation, then reinstall the final package so the comma is left running the tested build.

Expected:
- `/data/commaview` removed after uninstall.
- ports closed after uninstall.
- managed openpilot UI targets clean after uninstall.
- external revert backup exists under `/data/commaview-backups/onroad-ui-export-revert/`.
- final reinstall leaves runtime running and verifier clean.

---

### Task 9: Final review handoff

**Step 1: Summarize commits and verification**

Prepare final status with:

- Base: `v0.0.51-alpha`
- Final HEAD commit
- List of commits created by this plan
- Local verification commands and pass/fail outputs
- Device validation result, if run
- Whether repo is clean
- Whether branch is ahead of origin

**Step 2: Request one more code review**

Before pushing/releasing, run another focused review over the new delta:

```bash
git diff --stat v0.0.51-alpha..HEAD
git diff --name-status v0.0.51-alpha..HEAD
```

Ask reviewer to focus on:

- release overwrite/current pointer safety
- installer rollback and backup preservation
- verify all augmented path coverage
- red/stale contract cleanup
- CI dependency determinism
- provenance publication

**Step 3: Do not release until review is clean**

Hard stop: no push/tag/release until the final review findings are addressed or explicitly accepted by Rhyno.
