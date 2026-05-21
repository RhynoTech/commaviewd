# Onroad UI Export Transformer Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace brittle static upstream UI patching with a strict source transformer and update canaries to track early-drift branches.

**Architecture:** Keep flavor detection, offroad gating, backup, restart, and verifier flow in the shell wrapper. Add a Python transformer that installs the CommaView helper file and transforms only upstream-owned UI files using semantic anchors. Unknown or ambiguous layouts fail closed.

**Tech Stack:** Bash, Python 3 standard library, git, GitHub Actions.

---

### Task 1: Add transformer fixture tests for `ui_state.py`

**Files:**
- Create: `comma4/tests/onroad_ui_export_transformer_test.py`
- Read: `comma4/patches/openpilot/0001-commaview-ui-export-v2.patch`

**Step 1: Write failing tests**

Create temporary upstream trees with old and new `ui_state.py` snippets:

- old layout has inline `if time.monotonic() - self._param_update_time >= PARAM_UPDATE_TIME: self.update_params()` before `device.update()`.
- new layout has `_params_refresh_worker()` after `device.update()`.

Assert the transformer:

- adds the exporter import once;
- inserts exporter init/publish block after `device.update()`;
- is idempotent when run twice;
- fails if `device.update()` is missing;
- fails if two `device.update()` anchors exist in `UIState.update()`.

**Step 2: Run test to verify it fails**

Run:

```bash
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q
```

Expected: fail because transformer script does not exist.

**Step 3: Commit failing tests**

```bash
git add comma4/tests/onroad_ui_export_transformer_test.py
git commit -m "test: cover onroad UI export transformer anchors"
```

---

### Task 2: Implement `ui_state.py` transformation

**Files:**
- Create: `comma4/scripts/transform_onroad_ui_export.py`
- Modify: `comma4/tests/onroad_ui_export_transformer_test.py`

**Step 1: Implement minimal transformer CLI**

Add CLI:

```bash
python3 comma4/scripts/transform_onroad_ui_export.py --op-root <path> --flavor openpilot
```

Behavior:

- validate `--flavor` is `openpilot` or `sunnypilot`;
- transform `selfdrive/ui/ui_state.py` only for this task;
- locate `UIState.update()` by indentation-aware line scanning;
- locate exactly one `device.update()` inside that method;
- insert the exporter block after that line;
- add import near other `openpilot.selfdrive.ui...` imports;
- return non-zero with clear stderr on ambiguity.

**Step 2: Run tests**

```bash
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q
```

Expected: `ui_state.py` tests pass.

**Step 3: Commit**

```bash
git add comma4/scripts/transform_onroad_ui_export.py comma4/tests/onroad_ui_export_transformer_test.py
git commit -m "feat: transform ui_state onroad export hook"
```

---

### Task 3: Add transformer fixture tests for `augmented_road_view.py`

**Files:**
- Modify: `comma4/tests/onroad_ui_export_transformer_test.py`

**Step 1: Write failing tests**

Add fixture covering upstream `augmented_road_view.py` with:

- `super()._render(self._content_rect)`;
- `_switch_stream_if_needed()` followed by `_update_calibration()`;
- `self._model_renderer.set_transform(video_transform @ calib_transform)`.

Assert the transformer:

- inserts `self._update_commaview_camera_export()` after base render;
- adds `_update_commaview_camera_export()` before `_update_calibration()`;
- rewrites the model transform expression into `model_transform = video_transform @ calib_transform` and exports projection metadata;
- preserves equivalent `set_transform(model_transform)` behavior;
- is idempotent;
- fails on missing or duplicated anchors.

**Step 2: Run test to verify it fails**

```bash
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q
```

Expected: augmented-road tests fail.

**Step 3: Commit failing tests**

```bash
git add comma4/tests/onroad_ui_export_transformer_test.py
git commit -m "test: cover augmented road export transformer"
```

---

### Task 4: Implement `augmented_road_view.py` transformation

**Files:**
- Modify: `comma4/scripts/transform_onroad_ui_export.py`
- Modify: `comma4/tests/onroad_ui_export_transformer_test.py`

**Step 1: Implement strict augmented road transform**

Rules:

- find exactly one `super()._render(self._content_rect)` and insert camera export call after it;
- insert `_update_commaview_camera_export()` before `_update_calibration()` when missing;
- find exactly one `self._model_renderer.set_transform(video_transform @ calib_transform)`;
- replace with model transform assignment, unchanged `set_transform(model_transform)`, and projection export block;
- fail closed when transform expression differs from the expected safe anchor.

**Step 2: Run tests**

```bash
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q
```

Expected: all transformer fixture tests pass.

**Step 3: Commit**

```bash
git add comma4/scripts/transform_onroad_ui_export.py comma4/tests/onroad_ui_export_transformer_test.py
git commit -m "feat: transform augmented road export hooks"
```

---

### Task 5: Install helper source without static patching upstream files

**Files:**
- Create: `comma4/src/commaview_export.openpilot.py`
- Create: `comma4/src/commaview_export.sunnypilot.py`
- Modify: `comma4/scripts/transform_onroad_ui_export.py`
- Modify: `comma4/install.sh`

**Step 1: Extract helper source**

Extract `selfdrive/ui/commaview_export.py` payloads from existing openpilot and sunnypilot patch files into source templates.

**Step 2: Update transformer**

Transformer should copy the selected flavor template to:

```text
<op-root>/selfdrive/ui/commaview_export.py
```

Only overwrite if content differs and target is clean or force-repair flow has reset it.

**Step 3: Run syntax checks**

```bash
python3 -m py_compile comma4/scripts/transform_onroad_ui_export.py comma4/src/commaview_export.openpilot.py comma4/src/commaview_export.sunnypilot.py
```

Expected: success.

**Step 4: Commit**

```bash
git add comma4/scripts/transform_onroad_ui_export.py comma4/src/commaview_export.openpilot.py comma4/src/commaview_export.sunnypilot.py comma4/install.sh
git commit -m "feat: install onroad export helper from templates"
```

---

### Task 6: Wire transformer into apply/verify scripts

**Files:**
- Modify: `comma4/scripts/apply_onroad_ui_export_patch.sh`
- Modify: `comma4/scripts/verify_onroad_ui_export_patch.sh`
- Modify: `comma4/install.sh`

**Step 1: Update apply script**

Replace `git apply` lifecycle for upstream files with:

```bash
python3 "$INSTALL_DIR/scripts/transform_onroad_ui_export.py" --op-root "$OP_ROOT" --flavor "$flavor"
```

Preserve:

- offroad gating;
- dirty target refusal;
- `--force-repair` backup/reset behavior;
- UI restart behavior;
- status env write.

**Step 2: Update verify script**

Verifier should validate transformed markers in files, not patch applicability.

Status JSON should include:

```json
{
  "method": "transformer",
  "flavor": "openpilot|sunnypilot",
  "patchVerified": true,
  "repairNeeded": false
}
```

Keep existing runtime marker checks.

**Step 3: Run local contract tests**

```bash
./comma4/tests/onroad_ui_export_patch_contract_test.sh
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q
```

Expected: pass.

**Step 4: Commit**

```bash
git add comma4/scripts/apply_onroad_ui_export_patch.sh comma4/scripts/verify_onroad_ui_export_patch.sh comma4/install.sh comma4/tests/onroad_ui_export_transformer_test.py
git commit -m "feat: apply onroad export hooks with transformer"
```

---

### Task 7: Update canary and compatibility branch coverage

**Files:**
- Modify: `.github/workflows/commaviewd-canary-openpilot.yml`
- Modify: `.github/workflows/commaviewd-canary-sunnypilot.yml`
- Modify: `comma4/tests/onroad_ui_export_canary_applicability_test.sh`

**Step 1: Update early-drift canaries**

Set scheduled canary matrix to:

- openpilot: `master`
- sunnypilot: `dev`

Keep workflow names clear that these are early drift canaries.

**Step 2: Update local/CI applicability test**

Test transformer compatibility across:

Openpilot:

- `master`
- `nightly`
- `release-mici`
- `release-mici-staging`
- `release-tizi`
- `release-tizi-staging`

Sunnypilot:

- `dev`
- `staging`
- `release-mici`
- `release-mici-staging`
- `release-tizi`
- `release-tizi-staging`
- `master-tici`

**Step 3: Run workflow syntax check**

```bash
python3 - <<'PY'
import yaml
for path in [
  '.github/workflows/commaviewd-canary-openpilot.yml',
  '.github/workflows/commaviewd-canary-sunnypilot.yml',
]:
  with open(path) as f:
    yaml.safe_load(f)
print('workflow yaml ok')
PY
```

Expected: `workflow yaml ok`.

**Step 4: Commit**

```bash
git add .github/workflows/commaviewd-canary-openpilot.yml .github/workflows/commaviewd-canary-sunnypilot.yml comma4/tests/onroad_ui_export_canary_applicability_test.sh
git commit -m "ci: track early drift branches for onroad export canaries"
```

---

### Task 8: Run full verification

**Files:**
- No file changes expected.

**Step 1: Run focused tests**

```bash
python3 -m pytest comma4/tests/onroad_ui_export_transformer_test.py -q
./comma4/tests/onroad_ui_export_patch_contract_test.sh
./comma4/tests/onroad_ui_export_canary_applicability_test.sh
```

Expected: all pass.

**Step 2: Run repo verification pipeline if practical**

```bash
commaviewd/scripts/run-verification.sh
```

Expected: pass. If unavailable locally, document the blocker and focused test results.

**Step 3: Final commit if needed**

```bash
git status --short
git diff --check
```

Expected: clean status except intentional commits; no whitespace errors.
