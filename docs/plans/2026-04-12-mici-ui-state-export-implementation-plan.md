# MICI Upstream-Structured Runtime Export Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Rebuild the MICI runtime export around upstream source domains so `commaviewd` becomes the clear source of truth for onroad UI inputs.

**Architecture:** The runtime contract is defined and enforced in `commaviewd`, organized by upstream MICI source domains instead of legacy `control` / `scene` / `status` buckets. Android decoding and rendering adaptation are intentionally deferred until after the runtime shape is finalized and verified.

**Tech Stack:** Python patch overlays in `commaviewd`, shell contract tests, markdown audit docs.

---

## Scope

This plan is for **runtime-side MICI onroad export organization in `commaviewd`**.

It includes:
- freezing the audited upstream dependency inventory as the runtime contract source of truth
- defining upstream-structured runtime export groups
- removing compatibility-bucket thinking from the runtime contract
- updating both openpilot and sunnypilot runtime patches to emit the upstream-structured surface
- updating runtime contract tests to enforce the new structure
- writing runtime verification and a downstream handoff note

It does **not** include:
- Android-side decoder/client/storage changes
- parity adapter/renderer cutover
- schema/model sync work in the Android repo
- UI rendering improvements
- offroad/home/settings parity

## Canonical audit inputs

Treat these as required inputs before implementation:
- `/home/rhyno/Development/CommaView/docs/reports/2026-04-12-mici-mechanized-ui-state-audit.md`
- `/home/rhyno/Development/CommaView/docs/reports/2026-04-12-mici-final-ui-dependency-pass.md`
- `/home/rhyno/Development/CommaView/docs/reports/2026-04-12-mici-runtime-export-checklist.md`

## Contract shape rule

The runtime export must be grouped by upstream source domains:
- `uiStateOnroad`
- `selfdriveState`
- `carState`
- `controlsState`
- `onroadEvents`
- `driverMonitoringState`
- `driverStateV2`
- `modelV2`
- `radarState`
- `liveCalibration`
- `carOutput`
- `carControl`
- `liveParameters`
- `longitudinalPlan`
- `carParams`
- `deviceState`
- `roadCameraState`
- `pandaStatesSummary` or another explicitly named ignition/started summary if raw `pandaStates` is not exported verbatim

Hard rule: do **not** preserve `control` / `scene` / `status` as the primary runtime contract.

## High-risk fields that must be pinned explicitly

- `selfdriveState.alertHudVisual`
- `driverStateV2.leftDriverData/rightDriverData.faceOrientation`
- `longitudinalPlan.allowThrottle`
- `carParams.openpilotLongitudinalControl`
- `carParams.maxLateralAccel`
- `deviceState.deviceType`
- `roadCameraState.sensor`
- ignition/start semantics derived from `pandaStates`
- explicit handling of `started_frame` and `started_time`

## Task 1: Freeze the upstream-organized runtime contract inventory

**Files:**
- Read: `/home/rhyno/Development/CommaView/docs/reports/2026-04-12-mici-mechanized-ui-state-audit.md`
- Read: `/home/rhyno/Development/CommaView/docs/reports/2026-04-12-mici-final-ui-dependency-pass.md`
- Read: `/home/rhyno/Development/CommaView/docs/reports/2026-04-12-mici-runtime-export-checklist.md`
- Create: `/home/rhyno/Development/commaviewd/docs/reports/2026-04-12-mici-upstream-runtime-contract.md`

**Step 1: Write the runtime contract inventory**

Create a contract inventory grouped exactly by upstream source domain.

For each domain include:
- exported payload name
- exact upstream source fields
- whether the source is raw, normalized, or summarized
- whether freshness gating is runtime-local or exported
- whether the field is required now or intentionally out of scope

**Step 2: Verify the inventory against the audit set**

Run:
```bash
rg -n "alertHudVisual|faceOrientation|allowThrottle|maxLateralAccel|openpilotLongitudinalControl|deviceType|sensor|started_frame|started_time|pandaStates" /home/rhyno/Development/CommaView/docs/reports/2026-04-12-mici-*.md
```
Expected: every high-risk dependency appears in the audit set and is assigned to an upstream export group.

**Step 3: Commit**

```bash
git -C /home/rhyno/Development/commaviewd add docs/reports/2026-04-12-mici-upstream-runtime-contract.md
git -C /home/rhyno/Development/commaviewd commit -m "docs: define upstream-structured mici runtime contract"
```

## Task 2: Rewrite runtime exporters to emit upstream-shaped payloads

**Files:**
- Modify: `/home/rhyno/Development/commaviewd/comma4/patches/openpilot/0001-commaview-ui-export-v2.patch`
- Modify: `/home/rhyno/Development/commaviewd/comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch`

**Step 1: Refactor exporter helpers by upstream domain**

Split exporter logic so it mirrors the real sources:
- `_ui_state_onroad_payload(...)`
- `_selfdrive_state_payload(...)`
- `_car_state_payload(...)`
- `_controls_state_payload(...)`
- `_onroad_events_payload(...)`
- `_driver_monitoring_payload(...)`
- `_driver_state_v2_payload(...)`
- `_model_v2_payload(...)`
- `_radar_state_payload(...)`
- `_live_calibration_payload(...)`
- `_car_output_payload(...)`
- `_car_control_payload(...)`
- `_live_parameters_payload(...)`
- `_longitudinal_plan_payload(...)`
- `_car_params_payload(...)`
- `_device_state_payload(...)`
- `_road_camera_state_payload(...)`
- `_panda_states_summary_payload(...)`

**Step 2: Emit identical source-domain keys in both runtime flavors**

The openpilot and sunnypilot patches must export the same key names and structural layout wherever the audited common surface overlaps.

Flavor-specific extras are allowed only when:
- they are explicitly named as flavor-specific
- they do not mutate the shared source-domain meaning

**Step 3: Keep runtime-local behavior explicit**

If `started_frame`, `started_time`, or freshness checks remain local instead of exported, document that inside the patch comments.

**Step 4: Commit**

```bash
git -C /home/rhyno/Development/commaviewd add comma4/patches/openpilot/0001-commaview-ui-export-v2.patch comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch
git -C /home/rhyno/Development/commaviewd commit -m "feat: export mici ui state by upstream source domain"
```

## Task 3: Replace bucket-based runtime contract tests

**Files:**
- Modify: `/home/rhyno/Development/commaviewd/comma4/tests/onroad_ui_export_patch_contract_test.sh`
- Modify: `/home/rhyno/Development/commaviewd/comma4/tests/onroad_ui_export_canary_applicability_test.sh`

**Step 1: Write failing assertions for the new structure**

Assert the runtime patches expose upstream-domain groups and key fields, including:
- `uiStateOnroad`
- `selfdriveState.alertHudVisual`
- `driverStateV2` face orientation subset
- `controlsState` torque inputs
- `carOutput.actuatorsOutput.torque`
- `carControl.latActive`
- `liveParameters.roll`
- `longitudinalPlan.allowThrottle`
- `carParams.openpilotLongitudinalControl`
- `carParams.maxLateralAccel`
- `deviceState.deviceType`
- `roadCameraState.sensor`
- ignition/started summary payload

Also assert absence of the old primary bucket contract if applicable.

**Step 2: Run the patch contract test to verify failure**

Run:
```bash
bash /home/rhyno/Development/commaviewd/comma4/tests/onroad_ui_export_patch_contract_test.sh
```
Expected: FAIL on the first missing upstream-domain export.

**Step 3: Re-run runtime contract checks after patch changes**

Run:
```bash
bash /home/rhyno/Development/commaviewd/comma4/tests/onroad_ui_export_patch_contract_test.sh
bash /home/rhyno/Development/commaviewd/comma4/tests/onroad_ui_export_canary_applicability_test.sh
```
Expected: PASS.

**Step 4: Commit**

```bash
git -C /home/rhyno/Development/commaviewd add comma4/tests/onroad_ui_export_patch_contract_test.sh comma4/tests/onroad_ui_export_canary_applicability_test.sh
git -C /home/rhyno/Development/commaviewd commit -m "test: enforce upstream-structured mici runtime export"
```

## Task 4: Record runtime verification and downstream handoff

**Files:**
- Create: `/home/rhyno/Development/commaviewd/docs/reports/2026-04-12-mici-runtime-export-verification.md`

**Step 1: Write the verification note**

Include:
- final runtime export group list
- fields added or re-homed during the restructure
- tests run
- any remaining runtime-local behavior (`started_frame`, `started_time`, freshness gates)
- any intentionally deferred items
- explicit note that Android adaptation is deferred to a later plan

**Step 2: Commit**

```bash
git -C /home/rhyno/Development/commaviewd add docs/reports/2026-04-12-mici-runtime-export-verification.md
git -C /home/rhyno/Development/commaviewd commit -m "docs: record mici runtime export verification"
```

## Pre-execution gate

Before implementation starts, require proof that each audited dependency is classified as one of:
- exported directly in an upstream-domain payload
- normalized into `uiStateOnroad`
- kept runtime-local with explicit documentation and tests
- intentionally out of scope

If any dependency cannot be classified cleanly, stop and resolve it first.

## Exit criteria

This plan is complete when:
1. The runtime export contract is grouped by upstream source domains, not compatibility buckets.
2. Both runtime patches emit the same audited common structure.
3. Runtime contract tests enforce the upstream-domain structure.
4. High-risk fields are explicitly exported or explicitly documented as runtime-local.
5. Android adaptation remains deferred instead of leaking back into runtime design.
