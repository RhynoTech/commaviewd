# TICI Hook Installation Research Plan

## Status
Research phase plan for comma 3 / comma 3X support.

## Branch and release-base note

This branch is intended to be the TICI support research branch. The local checkout did not contain any Git tags or remotes when this work began, so there was no local `v*` release tag to branch from. I created `research/tici-hook-installation` from the available `work` branch and treated the checked-in `docs/release/v0.0.54-alpha-user-facing.md` as the last documented pre-transport-rewrite release baseline to preserve the release-era hook/installer design.

Before implementation work begins, re-anchor this branch to the actual release tag if the canonical repository has tags available. The intended base is the last actual runtime release before the UDP/video transport rewrite commits, not the current post-rewrite runtime line.

## Goal

Support comma 3 and comma 3X devices, whose openpilot UI code path is named `tici`, while keeping the initial scope limited to patch/hook installation for telemetry export into the existing CommaView sidecar path.

This phase does **not** redesign Android rendering, UDP video transport, or telemetry schemas. It answers where the hook belongs on TICI and which installer/transformer assumptions differ from the comma 4 `mici` path.

## Current CommaView hook model

The existing comma 4 installer uses a strict source transformer rather than a static patch. Its managed upstream targets are:

- `selfdrive/ui/commaview_export.py`
- `selfdrive/ui/ui_state.py`
- `selfdrive/ui/mici/onroad/augmented_road_view.py`
- `selfdrive/ui/onroad/augmented_road_view.py`

The transformer installs the CommaView helper, injects `_CommaViewSocketExporter` into `UIState.update()` immediately after `device.update()`, and augments the onroad camera view to export active camera/projection metadata. This is already close to a TICI-compatible installation model because TICI uses `selfdrive/ui/onroad/augmented_road_view.py` instead of the MICI-specific `selfdrive/ui/mici/onroad/augmented_road_view.py`.

## Upstream openpilot references checked

Research compared official `commaai/openpilot` branches:

| Device family | Branch checked | Relevant UI path |
| --- | --- | --- |
| comma 4 / MICI | `origin/release-mici` | `selfdrive/ui/mici/onroad/augmented_road_view.py` plus compatibility copy at `selfdrive/ui/onroad/augmented_road_view.py` |
| comma 3 / 3X / TICI | `origin/release-tici` | `selfdrive/ui/onroad/augmented_road_view.py` |
| comma 3 / 3X / TICI canary | `origin/master-tici` | `selfdrive/ui/onroad/augmented_road_view.py` |

Commands used for this audit:

```bash
git ls-remote --heads https://github.com/commaai/openpilot.git '*tici*' '*mici*'
git fetch --depth=1 origin release-mici release-tici master-tici
git ls-tree -r --name-only origin/release-mici selfdrive/ui
git ls-tree -r --name-only origin/release-tici selfdrive/ui
git show origin/release-mici:selfdrive/ui/ui_state.py
git show origin/release-tici:selfdrive/ui/ui_state.py
git show origin/release-mici:selfdrive/ui/mici/onroad/augmented_road_view.py
git show origin/release-tici:selfdrive/ui/onroad/augmented_road_view.py
```

## Hook location findings

### `selfdrive/ui/ui_state.py`

Both MICI and TICI have the same core hook point:

1. `class UIState`
2. `def update(self) -> None`
3. `device.update()` at the end of the update path

The existing CommaView injection point remains correct: publish immediately after `device.update()`. At that point `SubMaster` data, UI state fields, status, started state, and device-derived values have been refreshed for the current UI tick.

Observed difference: MICI currently has an additional params refresh thread in `UIState.update()`, while TICI's `UIState.update()` is simpler. This does not change the hook point, but it reinforces why the transformer should stay semantic-anchor based instead of using line numbers or hunk context.

### `selfdrive/ui/onroad/augmented_road_view.py`

TICI's onroad camera path is `selfdrive/ui/onroad/augmented_road_view.py`. It has the same essential anchors the current transformer knows about:

- base camera render anchor: `super()._render(rect)`
- calibration helper: `def _update_calibration(self):`
- active camera state: `self.stream_type == WIDE_CAM`
- available camera streams: `self.available_streams`
- model transform anchor: `self.model_renderer.set_transform(video_transform @ calib_transform)`

MICI's equivalent file uses `super()._render(self._content_rect)` and `self._model_renderer.set_transform(...)`; TICI uses `super()._render(rect)` and `self.model_renderer.set_transform(...)`. The current transformer already accounts for both render anchors and both renderer field names.

## Installation differences to account for

### 1. Device/platform detection

The installer should explicitly detect the upstream layout instead of assuming comma 4:

- MICI if `selfdrive/ui/mici/onroad/augmented_road_view.py` exists.
- TICI if `selfdrive/ui/onroad/augmented_road_view.py` exists and no MICI-specific target exists.

Branch names are useful for CI targets but should not select behavior on-device. The transformer should select based on file layout and semantic anchors.

### 2. Managed target list

The existing managed target list already includes both possible augmented-road paths. For TICI, only these should be changed:

- `selfdrive/ui/commaview_export.py`
- `selfdrive/ui/ui_state.py`
- `selfdrive/ui/onroad/augmented_road_view.py`

The MICI-only path should be ignored if absent. Verification output should identify the selected UI platform as `tici` or `mici` so the Android app and logs can distinguish install mode from runtime flavor.

### 3. Projection export details

TICI's render method receives `rect` and builds its cached matrix using `x` and `y` offsets inside `video_transform`. MICI uses `self._content_rect` more directly. The current transformer's projection block passes `content_rect=self._content_rect`; before enabling TICI support, validate that TICI's `_render()` always assigns `self._content_rect = rect` before `_update_calibration()`. If not, the transformer must pass the local `rect` or a normalized content rect variable on TICI.

This is the highest-risk installer detail because a hook can apply cleanly while exporting stale or wrong projection geometry.

### 4. Camera hardware constants

Both paths use `DEVICE_CAMERAS["tici", "ar0231"]` in the audited openpilot code, even on MICI's Python UI path. The hook should not infer device family from that constant. Use UI layout and process/runtime context instead.

### 5. Sidecar transport

The sidecar can continue consuming `uiStateOnroad` and projection/camera exports from `commaview_export.py`. No sidecar protocol change is required for the first TICI hook-installation milestone if the exporter payloads remain byte-compatible.

## Proposed implementation sequence

1. Add a `--platform auto|mici|tici` option to `transform_onroad_ui_export.py`.
2. In `auto`, select targets by existing files and semantic anchors, not branch names.
3. Make the projection insertion choose the content rectangle source that is valid for the selected target (`self._content_rect` for MICI; validated `self._content_rect` or local `rect` for TICI).
4. Extend `verify_onroad_ui_export_patch.sh` JSON with `uiPlatform: "mici" | "tici"` and selected target paths.
5. Add canary applicability tests against `commaai/openpilot@release-tici` and `commaai/openpilot@master-tici`.
6. Keep the sidecar payload contract unchanged until real-device telemetry confirms parity.

## Acceptance criteria for this phase

- The transformer applies cleanly to `commaai/openpilot@release-tici` without touching MICI-only files.
- The transformer applies cleanly to `commaai/openpilot@master-tici` or fails closed with a precise missing-anchor error.
- `UIState.update()` publishes after `device.update()` on TICI.
- TICI camera/projection export is installed after the base camera render and after the model transform is computed.
- Patch verification reports `uiPlatform=tici` and the exact transformed files.
- Sidecar receives the same helper-exported telemetry services it receives on MICI.

## Open questions for real-device validation

- Does comma 3X always expose both road and wide road streams in the same way as the audited TICI branches?
- Is TICI projection output aligned in Android when using the current `content_rect` field, or does it require passing the local render `rect`?
- Are there any performance regressions from publishing every UI tick on older comma 3 hardware?
- Should runtime logs expose `devicePlatform=tici` separately from `runtimeFlavor=OPENPILOT`?
