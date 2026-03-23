# Direct V2 CommaView Parity Cutover Design

> Supersedes `docs/plans/2026-03-22-hud-lite-hard-cutover-design.md`.

## Context
The HUD-lite cutover work proved the right core instinct: **UI-owned telemetry is the safe source of truth** and extra external subscribers are the dangerous part. What changed is product direction. We are no longer taking a staged HUD-lite v1 detour.

The approved target is a **direct cutover to the real v2 parity shape**:
- skip the HUD-lite transition path entirely
- accept a hard lockstep app/runtime release
- keep the no-extra-subscribers rule
- keep the UI process as the telemetry source of truth
- use a tiny upstream hook instead of maintaining a full fork

That means the design should stop pretending there is a compatibility window or a future second migration. There isn’t. We do the real shape once.

## Goals
1. Mirror comma/openpilot onroad UI semantics as closely as practical from UI-owned state.
2. Remove `commaviewd` as a direct subscriber to parity telemetry services.
3. Publish a small, explicit v2 export surface with three messages:
   - `commaViewControl`
   - `commaViewScene`
   - `commaViewStatus`
4. Keep Android as the consumer-side source of truth by preserving both:
   - explicit fork-specific export structs
   - a normalized common view used by current app rendering/state flows
5. Fail hard when the export contract is missing or mismatched.

## Non-goals
- No backward compatibility window between app and runtime.
- No fallback to legacy raw per-service subscriptions.
- No full openpilot/sunnypilot fork for CommaView.
- No new UI subscribers just to satisfy CommaView.
- No second-stage migration after a HUD-lite interim release.

## Alternatives considered

### 1. Finish HUD-lite v1 first, then migrate to v2 later
Rejected.

That creates two cutovers, two contract migrations, two rounds of app/runtime QA, and leaves misleading `hud-lite` naming everywhere while the real work still waits in the wings. Since hard lockstep release is allowed, staging buys us complexity instead of safety.

### 2. Maintain a full fork of upstream UI/schema code
Rejected.

It would reduce patch fragility short term but creates permanent merge debt. That is the exact tax we do not want to own.

### 3. Tiny upstream hook + direct v2 export + app-side normalization
Chosen.

This keeps the upstream surface small, keeps UI semantics authoritative, and lets Android preserve its current rendering pipeline through a normalized view while still retaining exact exported structures for parity-sensitive behavior.

## Chosen architecture

### 1. Three exported UI-owned services
We add three Cap'n Proto event types and services:
- `commaViewControl`
- `commaViewScene`
- `commaViewStatus`

They are published from upstream `selfdrive/ui/ui_state.py` using **only** values already available from the UI process and its existing `SubMaster`/UI state.

`commaviewd` subscribes only to those three services for telemetry. It does **not** subscribe to `carState`, `controlsState`, `modelV2`, `radarState`, `deviceState`, or the rest of the old raw parity matrix anymore.

### 2. Message responsibilities are intentionally split

#### `commaViewControl`
Owns fields that directly drive HUD/control semantics and alert behavior.

Examples of what belongs here:
- engaged / active / engageable state
- speed, cluster speed, cruise set speed, standstill
- steering angle / steering pressed / torque-related HUD inputs
- `carControl` / `carOutput` fields used for wheel, set-speed, torque-bar, and override presentation
- alert text / alert type / alert severity
- lane-departure / lead / blinker / hudControl indicators
- driver-monitor widget inputs that are rendered as HUD behavior
- explicit `started`, `isMetric`, and export version markers

Rule: if the app uses it to decide **what the HUD should say or do right now**, it lives here.

#### `commaViewScene`
Owns fields required to render the onroad scene geometry.

Examples of what belongs here:
- `modelV2` path, lane lines, road edges
- `radarState` lead data used for chevrons / clipping
- `liveCalibration` values used for projection
- road/wide-road camera frame metadata needed for camera-selection hysteresis and scene freshness
- explicit scene timestamps / export version markers

Rule: if the app uses it to decide **where pixels go on screen**, it lives here.

#### `commaViewStatus`
Owns app-facing contextual state that is not scene geometry and not immediate HUD control behavior.

Examples of what belongs here:
- `deviceState` subset used by the app
- `managerState` / `selfdriveState` subset needed for app status/presentation
- `pandaStates` summary / ignition context
- `driverStateV2` or other richer status-only signals not required for scene geometry
- freshness / ready bits the app can use for gating or debugging
- explicit status timestamps / export version markers

Rule: if it is useful context but not required to draw the current path or current HUD state, it lives here.

### 3. UI-owned source of truth remains non-negotiable
The upstream UI process already owns the correct semantic snapshot for onroad display. We keep that invariant.

The export hook must:
- publish from existing UI state only
- reuse existing `self.sm` subscriptions
- reuse existing UI-computed state like `self.status`, `self.started`, `self.is_metric`, camera state, etc.
- avoid adding any new `SubMaster` entries solely for CommaView

If a desired field would require a new upstream subscriber, it is out unless we first prove it is already UI-owned elsewhere.

### 4. Schema strategy: dedicated CommaView schema file, minimal upstream patch points
Approved strategy:
- use our own schema file where possible
- still patch `log.capnp`, `services.py`, and `ui_state.py` bootstrap where required

Concretely:
- add a dedicated schema file such as `cereal/commaview.capnp` in upstream patch context
- keep CommaView-specific structs there instead of stuffing more long-lived product schema into `custom.capnp`
- patch `cereal/log.capnp` to import that file and expose the three new `Event` union entries
- patch `cereal/services.py` to register the three services
- patch `selfdrive/ui/ui_state.py` to create a tiny `PubMaster` and publish the three messages

Android mirrors that by adding the same schema file under `CommaView/schemas/cereal/commaview.capnp` and extending local codegen/manifest tracking.

### 5. Lockstep contract, not compatibility theater
App and runtime ship together. So the contract should be explicit and loud.

Each exported message should carry an `exportVersion` field with value `2`.

Android behavior on mismatch:
- reject the payload
- mark parity telemetry unhealthy
- do not silently reinterpret old/new shapes

Runtime behavior on missing patch/export:
- mark the export unhealthy
- disable parity telemetry
- surface repair-needed state
- do not fall back to raw per-service subscriptions

### 6. Hybrid Android consumer model
Android should keep **two layers**:

#### Explicit export layer
Typed, fork-specific models that mirror the upstream export almost 1:1:
- `CommaViewControlV2`
- `CommaViewSceneV2`
- `CommaViewStatusV2`

These preserve exact semantics and make future parity work auditable.

#### Normalized common-view layer
A consumer-owned normalized layer synthesized from those explicit messages.

This layer can keep feeding the app’s existing state/rendering pipeline through familiar models such as:
- `CarState`
- `ControlsState`
- `CarControlState`
- `CarOutputState`
- `ModelState`
- `RadarState`
- `CalibrationState`
- `DeviceState`
- `DriverMonitoringStateData`
- `DriverStateV2State`
- `RoadCameraStateData`

That is the approved hybrid model: **preserve the fork-specific truth, then normalize it inside the app**.

### 7. `commaviewd` responsibilities after cutover
`commaviewd` stays small and dumb in the right places.

It should:
- verify/apply the upstream UI export patch lifecycle
- expose export health/repair status through control APIs
- bridge video as before
- forward only the three UI-export telemetry services to Android
- keep telemetry handling separate from video internally

It should not:
- reconstruct parity semantics from a pile of raw upstream services
- own the source of truth for overlay meaning
- carry a compatibility matrix for old HUD-lite vs new v2 messages

### 8. Naming cleanup is part of the cutover
Because there is no compatibility window, the control-plane naming should stop saying `hud-lite` once this lands.

Prefer names like:
- `onroad-ui-export`
- `commaViewControl/Scene/Status`
- `onroad-ui-export-status.json`
- `apply_onroad_ui_export_patch.sh`
- `verify_onroad_ui_export_patch.sh`

Keeping `hud-lite` labels after the direct v2 cutover would be lazy debt with a fake mustache on it.

## Patch lifecycle model
The upstream hook remains a tiny maintained patchset stored in `commaviewd`.

Lifecycle rules:
- install/upgrade applies the patch for openpilot or sunnypilot
- verification checks schema/service/publisher markers for all three services
- missing or stale patch produces explicit repair-needed state
- repair is allowed offroad only
- onroad repair is blocked
- Android surfaces the repair action from runtime API status

This is still the right compromise between “no fork” and “no maintenance.”

## Transport contract
The bridge keeps the raw metadata envelope path and forwards Cap'n Proto `Event` payloads.

Service index mapping should be fixed and shared by runtime + app:
- `0` → `commaViewControl`
- `1` → `commaViewScene`
- `2` → `commaViewStatus`

The JSON metadata path remains non-authoritative and should not be revived for parity telemetry.

## Acceptance criteria
The cutover is complete only when all of this is true:
1. `commaviewd` has no direct parity telemetry subscribers outside the three UI-export services.
2. Upstream patch assets create and verify `commaViewControl`, `commaViewScene`, and `commaViewStatus` on supported refs.
3. Android decodes the three explicit v2 messages from the pinned local schema snapshot.
4. Android synthesizes its normalized common view from those messages and current overlay rendering still works.
5. Lockstep mismatch fails loudly via version/health checks.
6. Patch loss after upstream update surfaces repair-needed instead of silent fallback.
7. All `hud-lite`-specific control-plane naming/contracts are either removed or intentionally renamed to the new export terminology.

## Summary
The direct v2 cutover is a **tiny upstream UI export hook plus an app-owned hybrid consumer model**. Upstream publishes three UI-owned messages (`control`, `scene`, `status`), `commaviewd` only forwards and health-checks them, and Android preserves exact exported semantics while normalizing them into the app’s existing overlay/state pipeline. That gets us parity without the toxic subscriber matrix and without signing up for a full fork.
