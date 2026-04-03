# 2026-04-03 Export-Difference Audit — openpilot vs sunnypilot (surgical)

## Scope

Audit target: MICI/runtime export differences relevant to CommaView direct UI export / honest raw export.

Refs audited:

| upstream | ref | sha |
|---|---|---|
| openpilot | `release-mici-staging` | `0f6971a94bababc56cc2cbe4fc001249c85541f2` |
| openpilot | `nightly` | `1a72c090e0e464a6922b7988e2b4aaa927892b2f` |
| sunnypilot | `staging` | `0102046b89ca8e6a30654fd3d6095d334346a9bf` |
| sunnypilot | `dev` | `d12485cde21885622de3a4e0dd307040065ccc78` |

Exact files audited:

- `cereal/car.capnp`
- `cereal/log.capnp`
- `cereal/services.py`
- `cereal/custom.capnp` (sunnypilot)
- `selfdrive/ui/ui_state.py`
- `selfdrive/ui/mici/onroad/alert_renderer.py`
- `selfdrive/ui/mici/onroad/model_renderer.py`
- `selfdrive/ui/mici/onroad/hud_renderer.py`
- `selfdrive/ui/mici/onroad/confidence_ball.py`
- `selfdrive/ui/sunnypilot/ui_state.py` (sunnypilot)
- `selfdrive/ui/sunnypilot/mici/onroad/model_renderer.py` (sunnypilot)
- `selfdrive/ui/sunnypilot/mici/onroad/confidence_ball.py` (sunnypilot)
- `selfdrive/ui/sunnypilot/onroad/speed_limit.py` (sunnypilot)
- `selfdrive/ui/sunnypilot/onroad/blind_spot_indicators.py` (sunnypilot)
- local export patch refs:
  - `comma4/patches/openpilot/0001-commaview-ui-export-v2.patch`
  - `comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch`
- local runtime/export contract refs:
  - `commaviewd/src/bridge_runtime.cc`
  - `commaviewd/src/telemetry_policy.h`
  - `commaviewd/docs/ai/telemetry-raw-only-deep-dive.md`

## Parity notes

- **openpilot `release-mici-staging` and `nightly` are functionally the same for this audit.** Audited export-relevant files were identical except for unrelated upstream churn in `cereal/log.capnp`, a tiny texture draw change in MICI alert renderer, and a wheel icon sizing tweak in MICI HUD.
- **sunnypilot `staging` and `dev` are identical for every exact audited file above.** No audited export-relevant drift between those two refs.

## Export-difference matrix

| item | openpilot support (`release-mici-staging` / `nightly`) | sunnypilot support (`staging` / `dev`) | clean export source | recommendation |
|---|---|---|---|---|
| `runtimeFlavor` | **Only via direct export patch.** Stock upstream has no runtime flavor field. Local patch hardcodes `COMMAVIEW_RUNTIME_FLAVOR = "OPENPILOT"` in `selfdrive/ui/ui_state.py` and publishes `commaViewStatus.runtimeFlavor`. | **Only via direct export patch.** Same mechanism, but `"SUNNYPILOT"`. | Direct export only: patched `commaViewStatus.runtimeFlavor`. Not honestly derivable from current raw-only runtime services. | Keep this as a normalized export field. Do **not** infer from random service presence on Android. |
| lat/long mode source / honest exportability | **Good for raw actuator truth, not UI mode semantics.** `cereal/car.capnp` has `CarControl.latActive` and `longActive`; local patch exports both via `commaViewControl`. Stock openpilot has no `LAT_ONLY` / `LONG_ONLY` UI status split. | **Needs sunnypilot-specific state.** `carControl.latActive/longActive` still exist, but MICI/UI mode semantics come from `selfdrive/ui/sunnypilot/ui_state.py` combining `selfdriveState.enabled` with `selfdriveStateSP.mads.*` into `engaged` / `lat_only` / `long_only` / `override`. Current direct export patch does **not** export that mode enum. | openpilot: `carControl.latActive` + `carControl.longActive` is honest enough. sunnypilot: needs `selfdriveStateSP` (`cereal/custom.capnp`) plus `selfdriveState` logic, or a normalized exported mode enum. | Next patch wave should add a normalized `statusMode`/`controlMode` enum to direct export. Do **not** pretend sunnypilot `lat_only` / `long_only` can be reconstructed honestly from `latActive` / `longActive` alone. |
| blindspot indicators | **Partial only.** `carState.leftBlindspot/rightBlindspot` exist, and MICI uses blindspot imagery only for `laneChangeBlocked` alert selection in `selfdrive/ui/mici/onroad/alert_renderer.py`. No dedicated persistent blindspot widget. | **Full MICI support.** `selfdrive/ui/sunnypilot/onroad/blind_spot_indicators.py` renders persistent indicators from `carState.leftBlindspot/rightBlindspot`, gated by the `BlindSpot` param; sunnypilot MICI HUD also hides wheel UI when blindspot is active. | Raw `carState.leftBlindspot/rightBlindspot` is the clean common source. Sunnypilot additionally depends on the `BlindSpot` param for enablement. | Export blindspot booleans as common fields, but treat persistent blindspot UI as sunnypilot-only unless a separate enable/toggle field is exported. |
| camera offset / camera policy inputs | **Partial.** Stock MICI model renderer does **not** apply `CameraOffset`; it uses raw `modelV2` geometry directly. Local direct patch can still export `scene.calibration.cameraOffset`, active camera, intrinsics, and `wideFromDeviceEuler`-based `cameraRpyOffset` from `selfdrive/ui/ui_state.py`. | **Full.** sunnypilot MICI model renderer explicitly applies `CameraOffset` to path, lane lines, road edges, and lead projection. It also shares the same wide/road camera policy inputs (`experimentalMode`, `wideRoadCameraState`, `deviceState`, `liveCalibration.wideFromDeviceEuler`) used by the local direct patch. | Camera policy: direct export scene is clean (`activeCamera`, intrinsics, `cameraRpyOffset`). Camera offset semantics: clean on sunnypilot only if `CameraOffset` is exported; raw-only current runtime does not ship params. | Keep `scene.calibration.cameraOffset` and `activeCamera` in direct export. If raw-only parity matters, add a tiny param snapshot/export for `CameraOffset`; otherwise sunnypilot path placement will drift. |
| rainbow-path relevant model/scene behavior | **Not supported.** Stock openpilot MICI path rendering has no rainbow mode branch. | **Supported.** `selfdrive/ui/sunnypilot/ui_state.py` reads `RainbowMode`; `selfdrive/ui/mici/onroad/model_renderer.py` switches to `RainbowPath.draw_rainbow_path(...)`; geometry still comes from the same projected path points. | Geometry is already in `modelV2` / direct scene path. The missing piece is just a style toggle (`RainbowMode`). | Do not add fake geometry fields. If Android wants parity, export a simple style flag for rainbow-path enablement on sunnypilot only. |
| speed-limit-pre-active state / icon behavior | **Not supported.** No equivalent audited MICI/openpilot file path or mode logic. Mark as absent, not “hidden parity.” | **Supported and clearly sunnypilot-specific.** `selfdrive/ui/sunnypilot/onroad/speed_limit.py` uses `longitudinalPlanSP.speedLimit.assist.state`, `resolver.speedLimitFinalLast`, `resolver.source`, `liveMapDataSP`, `carState.vCruiseCluster`, and `controlsState.vCruiseDEPRECATED`; `speedLimitPreActive` event is defined in `cereal/custom.capnp`; MICI alert renderer calls the pre-active icon helper. | Clean source is sunnypilot custom services: `longitudinalPlanSP` + `liveMapDataSP` + existing `carState` / `controlsState`. Current commaviewd raw-only service list does **not** include those SP services. Current direct export patch also does **not** normalize them. | Highest-value sunnypilot-only export gap. Add either a normalized speed-limit struct to direct export or add sunnypilot-only raw subscriptions for `longitudinalPlanSP` and `liveMapDataSP`. |
| confidence / status related mode inputs | **Partial.** Stock MICI confidence ball uses `modelV2.meta.disengagePredictions.brakeDisengageProbs` and `steerOverrideProbs`; current direct export patch exports alert and driver-monitoring state but **not** confidence inputs or `modelV2.meta`. | **Partial, but richer and not export-clean today.** Base engaged confidence still comes from `modelV2.meta`, but `LAT_ONLY` / `LONG_ONLY` animation and dot color depend on sunnypilot UI status logic plus `selfdriveStateSP`. Direct export patch omits that mode enum; current raw-only runtime omits `selfdriveStateSP`. | openpilot: raw `modelV2` + `selfdriveState` + `driverMonitoringState` is enough. sunnypilot: needs raw `modelV2` plus normalized status mode or raw `selfdriveStateSP`. | Add normalized `statusMode` to direct export first. Only add confidence-specific fields if Android cannot cheaply derive them from raw `modelV2.meta`. |

## Grounded findings by item

### 1) `runtimeFlavor`
- Grounded only in the local direct-export patch.
- Openpilot patch sets `COMMAVIEW_RUNTIME_FLAVOR = "OPENPILOT"`.
- Sunnypilot patch sets `COMMAVIEW_RUNTIME_FLAVOR = "SUNNYPILOT"`.
- Current raw-only runtime (`commaviewd/src/bridge_runtime.cc`, `commaviewd/docs/ai/telemetry-raw-only-deep-dive.md`) does not carry a runtime-flavor signal.

### 2) Lat/long mode source
- Common raw source exists in both forks: `CarControl.latActive` / `CarControl.longActive`.
- That is enough for actuator truth.
- It is **not** enough for sunnypilot UI truth, because sunnypilot derives `lat_only` / `long_only` from `selfdriveStateSP.mads` + `selfdriveState`, not from the raw actuator pair alone.

### 3) Blindspot
- Common raw booleans exist in both forks.
- Openpilot MICI uses them only indirectly during `laneChangeBlocked` alert icon selection.
- Sunnypilot adds persistent blindspot indicator rendering and uses the `BlindSpot` param as a feature gate.

### 4) Camera offset / camera policy
- Wide/road camera policy inputs are common enough to normalize.
- Camera offset is the real fork difference:
  - openpilot MICI does not apply it in the audited renderer,
  - sunnypilot MICI does.
- So exporting `cameraOffset` is harmless for openpilot but **required** for faithful sunnypilot overlay parity.

### 5) Rainbow path
- This is basically a **style toggle over existing path geometry**, not a new model source.
- Good news: no new geometry export is required.
- Bad news: pretending openpilot supports it would be bullshit.

### 6) Speed-limit pre-active
- This is the cleanest “sunnypilot-only” feature in the audit.
- It is grounded in custom schemas and custom UI helpers, not in upstream openpilot equivalents.
- Current commaviewd runtime does not subscribe to the needed sunnypilot SP services in raw-only mode, so Android cannot reproduce it honestly today.

### 7) Confidence / status inputs
- Openpilot confidence is mostly a `modelV2.meta` problem.
- Sunnypilot confidence is a `modelV2.meta` **plus** `status mode` problem.
- That means the minimum honest bridge addition is a normalized mode export, not a giant one-off UI dump.

## Recommended next patch wave

1. **Add a normalized status-mode field to direct export**
   - Candidate values: `DISENGAGED`, `ENGAGED`, `OVERRIDE`, `LAT_ONLY`, `LONG_ONLY`, `UNKNOWN`.
   - On openpilot, populate only the values it can honestly produce.
   - On sunnypilot, derive from the same logic now living in `selfdrive/ui/sunnypilot/ui_state.py`.

2. **Add a sunnypilot-only speed-limit export path**
   - Either:
     - extend `CommaViewStatus` / `CommaViewControl` with a normalized speed-limit struct, or
     - add raw subscriptions for `longitudinalPlanSP` and `liveMapDataSP` in commaviewd when runtime flavor is sunnypilot.
   - Do **not** try to synthesize this from openpilot fields that are not equivalent.

3. **Keep scene export responsible for camera policy + camera offset**
   - `activeCamera`
   - `cameraIntrinsics`
   - `cameraOffset`
   - `cameraRpyOffset`

4. **Add tiny style/config exports instead of bloating scene geometry**
   - `rainbowPathEnabled` (sunnypilot only)
   - optional blindspot feature/toggle if Android needs to mirror the UI gate

5. **Do not expand raw-only by default just to chase UI candy**
   - The raw-only bridge is currently narrow on purpose.
   - Only add sunnypilot SP services that unlock genuinely missing UI parity: `selfdriveStateSP`, `longitudinalPlanSP`, `liveMapDataSP`.

## Bottom line

- **Openpilot parity is mostly already covered by the existing direct patch plus raw `modelV2`.**
- **Sunnypilot parity is blocked by missing mode/status normalization and missing speed-limit custom-service export.**
- The next patch wave should be **small and explicit**: add status mode first, then speed-limit SP, then optional style flags. Anything broader risks fake parity and schema sludge.
