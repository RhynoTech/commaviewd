# MICI Runtime Export Verification

Date: 2026-04-12

## Summary

The runtime export in `commaviewd` now uses upstream-organized source domains as the primary socket contract instead of the old mixed compatibility buckets.

Old primary buckets removed from the contract:
- `commaViewControl`
- `commaViewScene`
- `commaViewStatus`

Current primary runtime export groups:
1. `uiStateOnroad`
2. `selfdriveState`
3. `carState`
4. `controlsState`
5. `onroadEvents`
6. `driverMonitoringState`
7. `driverStateV2`
8. `modelV2`
9. `radarState`
10. `liveCalibration`
11. `carOutput`
12. `carControl`
13. `liveParameters`
14. `longitudinalPlan`
15. `carParams`
16. `deviceState`
17. `roadCameraState`
18. `pandaStatesSummary`

## Key fields added or re-homed

### Normalized into `uiStateOnroad`
- `started`
- `ignition`
- `status`
- `isMetric`
- `alwaysOnDm`
- `hasLongitudinalControl`
- `startedFrame`
- `startedTime`

### Exported directly in source-domain payloads
- `selfdriveState.alertHudVisual`
- `controlsState.vCruiseDEPRECATED`
- `controlsState.lateralControlStateWhich`
- `controlsState.curvature`
- `controlsState.desiredCurvature`
- `controlsState.torqueBarValue`
- `driverStateV2.leftDriverData.faceOrientation`
- `driverStateV2.rightDriverData.faceOrientation`
- `carOutput.actuatorsOutput.torque`
- `carControl.latActive`
- `carControl.longActive`
- `liveParameters.roll`
- `longitudinalPlan.allowThrottle`
- `carParams.openpilotLongitudinalControl`
- `carParams.maxLateralAccel`
- `deviceState.deviceType`
- `roadCameraState.sensor`
- `pandaStatesSummary.ignitionLine`
- `pandaStatesSummary.ignitionCan`
- `pandaStatesSummary.ignition`

## Runtime contract / bridge changes
- updated the upstream patch assets for both `openpilot` and `sunnypilot`
- expanded the bridge/runtime service map from 3 services to 18 services
- expanded runtime debug defaults and policy defaults to the same 18-service map
- updated patch verification, patch canary applicability, and runtime contract tests to enforce the new structure
- updated the startup patch verifier to validate the upstream-organized payload helpers and reject legacy bucket markers

## Verification run

### Patch contract and runtime contract checks
```bash
bash comma4/tests/onroad_ui_export_patch_contract_test.sh
bash commaviewd/tests/runtime_debug_policy_contract_test.sh
python3 -m pytest -q comma4/tests/commaview_api_runtime_debug_config_test.py
OP_ROOT=/home/rhyno/.cache/ci-ref-checkouts/openpilot-release-mici-staging COMMAVIEWD_SKIP_ARM=1 ./commaviewd/scripts/run-unit-tests.sh
```
Result:
- PASS: upstream-organized socket UI export patch contract present
- PASS: runtime debug policy contract checks passed
- PASS: `comma4/tests/commaview_api_runtime_debug_config_test.py` (`6 passed`)
- PASS: `commaviewd` unit tests passed

### Real-ref patch applicability checks
```bash
bash comma4/tests/onroad_ui_export_canary_applicability_test.sh
```
Result:
- PASS on `openpilot release-mici-staging`
- PASS on `openpilot nightly`
- PASS on `sunnypilot staging`
- PASS on `sunnypilot dev`

### Startup verifier spot check
```bash
COMMAVIEWD_INSTALL_DIR=<temp install root> \
COMMAVIEWD_OP_ROOT=/home/rhyno/.cache/ci-ref-checkouts/openpilot-release-mici-staging \
comma4/scripts/verify_onroad_ui_export_patch.sh --json
```
Result:
- `patchVerified=true`
- `serviceMarkerCount=18`

## Remaining runtime-local behavior
- freshness gates based on `recv_frame[...] >= started_frame`
- startup-time staleness handling and default/fallback behavior before publishers are live
- torque-bar internal derivation helper implementation details

Note:
- `startedFrame` and `startedTime` are now exported through `uiStateOnroad`, but freshness gating behavior still remains runtime-local.

## Intentionally deferred
- Android decoding and storage adaptation
- Android `TelemetryClient` remapping
- Android `StateFlow` / renderer consumer updates
- any app-side compatibility shaping of the new runtime contract

Android adaptation remains deferred on purpose. The runtime contract in `commaviewd` is now the source of truth, and downstream app work should adapt to it later instead of dragging the runtime back into mixed compatibility buckets.
