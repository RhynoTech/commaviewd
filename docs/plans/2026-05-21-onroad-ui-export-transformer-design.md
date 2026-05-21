# Onroad UI Export Transformer Design

## Status
Approved for implementation.

## Context
The `commaviewd-canary-openpilot` workflow failed because the static direct v2 onroad UI export patch no longer applies to upstream `selfdrive/ui/ui_state.py`. Upstream added a background params refresh path and moved nearby context, while the data surfaces CommaView uses remain present.

Static patches are brittle for active upstream/fork development. CommaView needs a safer patching model for upstream-owned UI files while keeping fail-closed behavior on unknown layouts.

## Decision
Replace static patch application for upstream-owned files with a strict source transformer:

- Transform `selfdrive/ui/ui_state.py`.
- Transform `selfdrive/ui/mici/onroad/augmented_road_view.py`.
- Continue installing CommaView-owned `selfdrive/ui/commaview_export.py` from a checked-in source/template.
- Select repo flavor (`openpilot`/`sunnypilot`) as today, but do not select by branch, version, or SHA range.
- Fail closed on missing, duplicated, or ambiguous semantic anchors.

## Transformer rules

### `ui_state.py`

The transformer must:

1. Ensure this import exists once:
   `from openpilot.selfdrive.ui.commaview_export import _CommaViewSocketExporter, COMMAVIEW_RUNTIME_FLAVOR`
2. Find exactly one `class UIState`.
3. Find exactly one `def update(self) -> None:` or equivalent `def update(self):` inside `UIState`.
4. Find exactly one `device.update()` call inside that method.
5. Insert the exporter init/publish block immediately after `device.update()` when missing.
6. Do nothing if the expected import and publish block are already present.

### `augmented_road_view.py`

The transformer must:

1. Find exactly one base camera render anchor:
   `super()._render(self._content_rect)`
2. Insert `self._update_commaview_camera_export()` after that anchor when missing.
3. Ensure `_update_commaview_camera_export()` exists once, inserted before `_update_calibration()` when missing.
4. Find exactly one model transform anchor:
   `self._model_renderer.set_transform(video_transform @ calib_transform)`
5. Replace that anchor with an equivalent `model_transform = video_transform @ calib_transform`, `set_transform(model_transform)`, then CommaView projection export.
6. Preserve upstream behavior: `set_transform()` receives the same computed matrix.
7. Do nothing when the transformed block is already present.

## Safety behavior

- Default mode must refuse to modify dirty upstream target files.
- `--force-repair` may reset only managed targets after writing backups.
- Unknown layouts must exit non-zero with a clear reason and file fingerprint.
- After transformation, run Python syntax checks for modified Python files.
- Then run `verify_onroad_ui_export_patch.sh`.
- Status JSON should report method `transformer`, flavor, target paths, fingerprints, and repair state.

## Canary policy

Canaries should catch drift before release branches move:

- Openpilot canary target: `commaai/openpilot@master`.
- Sunnypilot canary target: `sunnypilot/sunnypilot@dev`.

Compatibility/applicability tests should still cover release and staging lanes:

- openpilot: `master`, `nightly`, `release-mici`, `release-mici-staging`, `release-tizi`, `release-tizi-staging`.
- sunnypilot: `dev`, `staging`, `release-mici`, `release-mici-staging`, `release-tizi`, `release-tizi-staging`, and `master-tici` while it remains relevant.

Do not infer transformer selection from branch names or SHA ordering. Branch names are test targets only.

## Alternatives considered

### Static patch variants
Simpler short-term, but creates recurring maintenance every time upstream moves nearby context.

### SHA/version-gated patch selection
Rejected. Git SHA ordering is meaningless, and ancestry checks are unreliable across forks, rebases, and release-stamped branches.

### Full AST rewrite
Rejected for now. It is heavier, can disturb formatting/comments, and adds more machinery than needed.

## Consequences

Positive:

- More resilient to harmless upstream context churn.
- Better diagnostics than generic hunk failures.
- Keeps device behavior fail-closed on unknown layouts.

Negative:

- More code and tests than static patches.
- Transformer must stay conservative; over-clever rewriting would be dangerous.
