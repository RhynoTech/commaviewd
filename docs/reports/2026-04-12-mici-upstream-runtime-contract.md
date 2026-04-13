# MICI Upstream Runtime Contract Inventory

Date: 2026-04-12

## Purpose

This document freezes the runtime export contract for MICI onroad UI inputs as an upstream-organized source-domain surface.

The contract source of truth lives in `commaviewd`.

## Domain inventory

### `uiStateOnroad`
- Source type: normalized and summarized from `ui_state` cache fields plus audited upstream derivations
- Required fields:
  - `started`
  - `ignition`
  - `status`
  - `isMetric`
  - `alwaysOnDm`
  - `hasLongitudinalControl`
  - `startedFrame`
  - `startedTime`
- Notes:
  - `started_frame` and `started_time` may remain runtime-local if explicitly documented and tested.
  - `ignition` is derived from `pandaStates` unless a more explicit started-summary is exported.

### `selfdriveState`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `enabled`
  - `active`
  - `engageable`
  - `alertText1`
  - `alertText2`
  - `alertType`
  - `alertStatus`
  - `alertSize`
  - `alertHudVisual`
  - `experimentalMode`
- Notes:
  - `alertHudVisual` is a pinned high-risk field.

### `carState`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `vEgo`
  - `vEgoCluster`
  - `vCruiseCluster`
  - `standstill`
  - `steeringAngleDeg`
  - `steeringPressed`
  - `leftBlinker`
  - `rightBlinker`
  - `leftBlindspot`
  - `rightBlindspot`

### `controlsState`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `vCruiseDEPRECATED`
  - `lateralControlStateWhich`
  - `curvature`
  - `desiredCurvature`
- Notes:
  - torque-bar math depends on these exact inputs, not a collapsed substitute.

### `onroadEvents`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `events[].name`
  - `events[].enable`
  - `events[].noEntry`
  - `events[].warning`
  - `events[].userDisable`
  - `events[].softDisable`
  - `events[].immediateDisable`
  - `events[].preEnable`
  - `events[].permanent`
  - `events[].overrideLateral`
  - `events[].overrideLongitudinal`

### `driverMonitoringState`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `faceDetected`
  - `isDistracted`
  - `isRHD`
  - `poseYawOffset`
  - `posePitchOffset`
  - `poseYawValidCount`
  - `posePitchValidCount`
  - `isLowStd`
  - `isActiveMode`

### `driverStateV2`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `leftDriverData.faceOrientation`
  - `rightDriverData.faceOrientation`
- Optional later if needed:
  - face orientation stds / position fields
- Notes:
  - `faceOrientation` is a pinned high-risk field.

### `modelV2`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `frameId`
  - `frameIdExtra`
  - `frameAge`
  - `frameDropPerc`
  - `timestampEof`
  - `position`
  - `laneLines`
  - `laneLineProbs`
  - `laneLineStds`
  - `roadEdges`
  - `roadEdgeStds`
  - `meta.disengagePredictions`
  - `acceleration.x`

### `radarState`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `leadOne`
  - `leadTwo`

### `liveCalibration`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `rpyCalib`
  - `height`
  - `calStatus`
  - `calPerc`
  - `wideFromDeviceEuler` when available

### `carOutput`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `actuatorsOutput.torque`

### `carControl`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `latActive`
  - `longActive`

### `liveParameters`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `roll`

### `longitudinalPlan`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `allowThrottle`
- Notes:
  - must be exported from the actual longitudinal publisher path, not inferred.

### `carParams`
- Source type: raw-ish publisher payload subset or normalized cached copy
- Required fields:
  - `openpilotLongitudinalControl`
  - `maxLateralAccel`
- Notes:
  - both are pinned high-risk fields.

### `deviceState`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `started`
  - `deviceType`

### `roadCameraState`
- Source type: raw-ish publisher payload subset
- Required fields:
  - `sensor`
  - `frameId` if needed by downstream frame-sync consumers

### `pandaStatesSummary`
- Source type: summarized from `pandaStates`
- Required fields:
  - `ignitionLine`
  - `ignitionCan`
  - `ignition`
- Notes:
  - if the runtime exports a more minimal started/ignition summary instead, the payload name must still be explicit that it is a summary.

## Explicitly out of first-scope runtime export
- `ui_state.light_sensor` from `wideRoadCameraState.exposureValPercent`
- offroad/home/settings params and control actions
- home screen network state

## Runtime-local allowed with documentation
- `started_frame`
- `started_time`
- freshness gates that exist only to reject stale pre-onroad frames

## Contract rules
1. Do not reintroduce `commaViewControl`, `commaViewScene`, or `commaViewStatus` as primary contract groups.
2. Do not hide source-domain fields behind mixed consumer buckets.
3. Do not substitute synthetic sources for `allowThrottle`, `openpilotLongitudinalControl`, or `maxLateralAccel`.
4. Do not collapse `deviceType` and `roadCameraState.sensor` into opaque camera labels without preserving the source fields too.
5. If a source remains runtime-local, say so explicitly in verification.
