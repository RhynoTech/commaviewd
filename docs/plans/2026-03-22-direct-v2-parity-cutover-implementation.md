# Direct V2 CommaView Parity Cutover Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the HUD-lite transition path with the approved direct v2 `commaViewControl` / `commaViewScene` / `commaViewStatus` cutover across `commaviewd` and `CommaView`.

**Architecture:** Upstream UI publishes three tiny UI-owned export messages from existing UI state, `commaviewd` only forwards and health-checks those messages, and Android preserves both the exact exported structs and a normalized common view for the current overlay/state pipeline. The release is lockstep, so the code should fail loudly on contract mismatch instead of carrying compatibility shims.

**Tech Stack:** Cap'n Proto schemas, openpilot/sunnypilot patch files, C++ runtime bridge/control API, Kotlin Android flows/tests, Gradle, shell contract tests.

---

### Task 1: Pin the shared v2 schema contract in the Android snapshot

**Files:**
- Create: `/home/pear/CommaView/schemas/cereal/commaview.capnp`
- Modify: `/home/pear/CommaView/schemas/cereal/log.capnp`
- Modify: `/home/pear/CommaView/schemas/manifest.json`
- Modify: `/home/pear/CommaView/scripts/generate-capnp-bindings.sh`
- Test: `/home/pear/CommaView/scripts/verify-schema-snapshot.sh`
- Test: `/home/pear/CommaView/scripts/verify-generated-bindings-reproducible.sh`

**Step 1: Write the failing schema snapshot first**

Add `/home/pear/CommaView/schemas/cereal/commaview.capnp` with the three structs and explicit version field:

```capnp
@0xe3d2f2c0b57c9f11;

struct CommaViewControl {
  exportVersion @0 :UInt16;
}

struct CommaViewScene {
  exportVersion @0 :UInt16;
}

struct CommaViewStatus {
  exportVersion @0 :UInt16;
}
```

Patch `schemas/cereal/log.capnp` so `Event` imports `commaview.capnp` and exposes:

```capnp
commaViewControl @150 :CommaView.CommaViewControl;
commaViewScene   @151 :CommaView.CommaViewScene;
commaViewStatus  @152 :CommaView.CommaViewStatus;
```

**Step 2: Make the manifest fail loudly until it is updated**

Run:

```bash
cd /home/pear/CommaView
./scripts/verify-schema-snapshot.sh
```

Expected: `FAIL: sha mismatch` or `FAIL: missing schema file schemas/cereal/commaview.capnp`.

**Step 3: Update manifest + generator minimally**

Extend `schemas/manifest.json` with the new file hash and update `scripts/generate-capnp-bindings.sh` to:
- require `schemas/cereal/commaview.capnp`
- copy it into the temp schema root
- add Java annotations if needed
- compile it alongside `log.capnp`

Target shape inside the script:

```bash
for path in ... "$SCHEMA_ROOT/cereal/commaview.capnp"; do
  [[ -f "$path" ]] || { echo "Missing schema dependency: $path"; exit 1; }
done
...
cp "$SCHEMA_ROOT/cereal/commaview.capnp" "$TMP_ROOT/cereal/commaview.capnp"
...
for pair in ... "commaview:CommaviewCapnp"; do
...
(cd "$TMP_ROOT" && capnp compile ... cereal/commaview.capnp cereal/log.capnp ...)
```

**Step 4: Run the schema checks**

Run:

```bash
cd /home/pear/CommaView
./scripts/verify-schema-snapshot.sh
./scripts/verify-generated-bindings-reproducible.sh
```

Expected: both print `PASS:`.

**Step 5: Commit**

```bash
cd /home/pear/CommaView
git add schemas/cereal/commaview.capnp schemas/cereal/log.capnp schemas/manifest.json scripts/generate-capnp-bindings.sh
git commit -m "feat(schema): add v2 commaview export contract"
```

### Task 2: Replace the HUD-lite upstream patch with the real v2 UI export hook

**Files:**
- Create/replace: `/home/pear/commaviewd/comma4/patches/openpilot/0001-commaview-ui-export-v2.patch`
- Create/replace: `/home/pear/commaviewd/comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch`
- Test: `/home/pear/commaviewd/comma4/tests/hud_lite_patch_contract_test.sh`
- Test: `/home/pear/commaviewd/comma4/tests/hud_lite_canary_applicability_test.sh`

**Step 1: Make the contract test fail on the old patch names/content**

Update `comma4/tests/hud_lite_patch_contract_test.sh` so it expects the new patch names plus all three service markers:

```bash
grep -Fq 'commaViewControl' "$PATCH"
grep -Fq 'commaViewScene' "$PATCH"
grep -Fq 'commaViewStatus' "$PATCH"
```

Run:

```bash
cd /home/pear/commaviewd
bash comma4/tests/hud_lite_patch_contract_test.sh
```

Expected: FAIL while the old HUD-lite patch files are still in place.

**Step 2: Author the new patch files, not a full fork**

In both upstream patch files:
- add `cereal/commaview.capnp`
- patch `cereal/log.capnp`
- patch `cereal/services.py`
- patch `selfdrive/ui/ui_state.py`

The `ui_state.py` hunk should look conceptually like:

```python
self._commaview_pm = messaging.PubMaster([
  "commaViewControl",
  "commaViewScene",
  "commaViewStatus",
])
...
self._publish_commaview_control()
self._publish_commaview_scene()
self._publish_commaview_status()
```

And each publisher must populate `exportVersion = 2` and use only existing UI-owned state from `self.sm` / `self.status` / `self.started` / `self.is_metric`.

**Step 3: Update canary applicability expectations**

Make `comma4/tests/hud_lite_canary_applicability_test.sh` assert the marker output proves all three services exist:

```bash
grep -Fq '"controlServicePresent":true' <<<"$output"
grep -Fq '"sceneServicePresent":true' <<<"$output"
grep -Fq '"statusServicePresent":true' <<<"$output"
```

**Step 4: Run the patch contract tests**

Run:

```bash
cd /home/pear/commaviewd
bash comma4/tests/hud_lite_patch_contract_test.sh
HUD_LITE_CANARY_TEST_KEEP=0 bash comma4/tests/hud_lite_canary_applicability_test.sh
```

Expected: both print `PASS:`.

**Step 5: Commit**

```bash
cd /home/pear/commaviewd
git add comma4/patches/openpilot/0001-commaview-ui-export-v2.patch \
        comma4/patches/sunnypilot/0001-commaview-ui-export-v2.patch \
        comma4/tests/hud_lite_patch_contract_test.sh \
        comma4/tests/hud_lite_canary_applicability_test.sh
git commit -m "feat(runtime): add direct v2 ui export patch"
```

### Task 3: Rename and tighten the patch lifecycle and control-plane status API

**Files:**
- Create: `/home/pear/commaviewd/comma4/scripts/apply_onroad_ui_export_patch.sh`
- Create: `/home/pear/commaviewd/comma4/scripts/verify_onroad_ui_export_patch.sh`
- Modify: `/home/pear/commaviewd/comma4/install.sh`
- Modify: `/home/pear/commaviewd/comma4/upgrade.sh`
- Modify: `/home/pear/commaviewd/commaviewd/src/control_mode.cpp`
- Modify: `/home/pear/commaviewd/commaviewd/tests/control_mode_api_contract_test.sh`
- Modify: `/home/pear/commaviewd/commaviewd/tests/hud_lite_ci_contract_test.sh`

**Step 1: Update the control-mode contract to fail on stale HUD-lite naming**

Change `commaviewd/tests/control_mode_api_contract_test.sh` to expect the new mode/status terms:

```bash
grep -Fq 'return "direct-v2-ui-export";' "$CONTROL_CPP"
grep -Fq '"onroadUiExport"' "$CONTROL_CPP"
grep -Fq '/commaview/onroad-ui-export/status' "$CONTROL_CPP"
grep -Fq '/commaview/onroad-ui-export/repair' "$CONTROL_CPP"
```

Run:

```bash
cd /home/pear/commaviewd
bash commaviewd/tests/control_mode_api_contract_test.sh
```

Expected: FAIL until `control_mode.cpp` is updated.

**Step 2: Add the new apply/verify helpers and stop guessing flavors**

Copy the existing helper logic into the new script names, then update them to check for:
- `cereal/commaview.capnp`
- the three `log.capnp` event entries
- the three `services.py` entries
- the three `ui_state.py` publishers
- marker JSON keys like `controlServicePresent`, `sceneServicePresent`, `statusServicePresent`

**Step 3: Rewire install/upgrade and runtime status**

Update `comma4/install.sh`, `comma4/upgrade.sh`, and `commaviewd/src/control_mode.cpp` to use:
- `onroad-ui-export-status.json`
- `apply_onroad_ui_export_patch.sh`
- `verify_onroad_ui_export_patch.sh`
- `/commaview/onroad-ui-export/status`
- `/commaview/onroad-ui-export/repair`

Keep `/commaview/status`, but change its payload to embed the new nested object:

```json
{
  "telemetryMode": "direct-v2-ui-export",
  "onroadUiExport": { ... }
}
```

**Step 4: Update workflow contract coverage**

Edit `commaviewd/tests/hud_lite_ci_contract_test.sh` so CI is expected to:
- call the new verify/apply helper names
- surface `onroad-ui-export-status.json`
- stop asserting stale `hud-lite` strings

**Step 5: Run the runtime contract tests**

Run:

```bash
cd /home/pear/commaviewd
bash commaviewd/tests/control_mode_api_contract_test.sh
bash commaviewd/tests/hud_lite_ci_contract_test.sh
```

Expected: both print `PASS:`.

**Step 6: Commit**

```bash
cd /home/pear/commaviewd
git add comma4/scripts/apply_onroad_ui_export_patch.sh \
        comma4/scripts/verify_onroad_ui_export_patch.sh \
        comma4/install.sh comma4/upgrade.sh \
        commaviewd/src/control_mode.cpp \
        commaviewd/tests/control_mode_api_contract_test.sh \
        commaviewd/tests/hud_lite_ci_contract_test.sh
git commit -m "feat(runtime): rename patch lifecycle for direct v2 export"
```

### Task 4: Cut the bridge over to the three raw v2 services only

**Files:**
- Modify: `/home/pear/commaviewd/commaviewd/src/bridge_runtime.cc`
- Modify: `/home/pear/commaviewd/commaviewd/tests/raw_only_runtime_contract_test.sh`
- Test: `/home/pear/commaviewd/commaviewd/tests/unit_tests_pipeline_test.sh`

**Step 1: Make the raw-only bridge contract fail until it expects the new service trio**

Update `commaviewd/tests/raw_only_runtime_contract_test.sh` to assert:

```bash
assert_contains_fixed '"commaViewControl"' "$BRIDGE_CPP" 'missing control subscription'
assert_contains_fixed '"commaViewScene"' "$BRIDGE_CPP" 'missing scene subscription'
assert_contains_fixed '"commaViewStatus"' "$BRIDGE_CPP" 'missing status subscription'
assert_not_contains_fixed '"commaViewHudLite"' "$BRIDGE_CPP" 'HUD-lite service should be gone'
```

Run:

```bash
cd /home/pear/commaviewd
bash commaviewd/tests/raw_only_runtime_contract_test.sh
```

Expected: FAIL while the bridge still only references `commaViewHudLite`.

**Step 2: Change the service map, not the transport framing**

In `commaviewd/src/bridge_runtime.cc`:
- keep the v4 raw envelope shape
- replace the single HUD-lite subscription with the three new services
- fix service index order to `control=0`, `scene=1`, `status=2`
- keep the dedicated telemetry thread
- delete any remaining HUD-lite-only strings and assumptions

The desired structure is:

```cpp
constexpr std::array<const char*, 3> kTelemetryServices = {
  "commaViewControl",
  "commaViewScene",
  "commaViewStatus",
};
```

**Step 3: Run the runtime bridge checks**

Run:

```bash
cd /home/pear/commaviewd
bash commaviewd/tests/raw_only_runtime_contract_test.sh
bash commaviewd/tests/unit_tests_pipeline_test.sh
```

Expected: both print `PASS:`.

**Step 4: Commit**

```bash
cd /home/pear/commaviewd
git add commaviewd/src/bridge_runtime.cc commaviewd/tests/raw_only_runtime_contract_test.sh
git commit -m "feat(runtime): forward direct v2 ui export services"
```

### Task 5: Add explicit v2 decode models and normalize them inside `TelemetryClient`

**Files:**
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/model/OnroadParityTelemetry.kt`
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/connection/raw/RawTelemetryDecoder.kt`
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/connection/TelemetryClient.kt`
- Modify: `/home/pear/CommaView/app/src/test/java/com/commaview/app/connection/RawV4TelemetryFixtures.kt`
- Modify: `/home/pear/CommaView/app/src/test/java/com/commaview/app/connection/raw/CapnpRawTelemetryDecoderTest.kt`
- Modify: `/home/pear/CommaView/app/src/test/java/com/commaview/app/connection/TelemetryClientRawCapnpDecodingTest.kt`
- Modify: `/home/pear/CommaView/app/src/test/java/com/commaview/app/connection/TelemetryClientHudLiteDecodingTest.kt`
- Modify: `/home/pear/CommaView/app/src/test/java/com/commaview/app/review/HudLiteCutoverSourceContractTest.kt`

**Step 1: Write failing fixtures/tests for all three services**

Expand `RawV4TelemetryFixtures.kt` and `CapnpRawTelemetryDecoderTest.kt` so the decoder is pinned to:
- service index `0` → `commaViewControl`
- service index `1` → `commaViewScene`
- service index `2` → `commaViewStatus`
- `exportVersion == 2`

Expected result shape:

```kotlin
sealed interface RawTelemetryDecodeResult {
  data class Control(val telemetry: CommaViewControlV2) : RawTelemetryDecodeResult
  data class Scene(val telemetry: CommaViewSceneV2) : RawTelemetryDecodeResult
  data class Status(val telemetry: CommaViewStatusV2) : RawTelemetryDecodeResult
}
```

Run:

```bash
cd /home/pear/CommaView
./gradlew test --tests 'com.commaview.app.connection.raw.CapnpRawTelemetryDecoderTest'
```

Expected: FAIL while the decoder still only knows `commaViewHudLite`.

**Step 2: Add the explicit v2 models**

In `OnroadParityTelemetry.kt`, keep `HudLiteTelemetry` only if other code still temporarily references it during the cutover commit sequence; otherwise remove it and add:

```kotlin
data class CommaViewControlV2(val exportVersion: Int = 2, ...)
data class CommaViewSceneV2(val exportVersion: Int = 2, ...)
data class CommaViewStatusV2(val exportVersion: Int = 2, ...)
```

Preserve fork-specific field names where practical. Do not prematurely squash everything into generic overlay DTOs.

**Step 3: Update the raw decoder**

Modify `RawTelemetryDecoder.kt` so it:
- decodes the three event union cases from generated Cap'n Proto classes
- rejects unknown service-index/event pairings
- rejects `exportVersion != 2`
- returns `Malformed` on decode exceptions

**Step 4: Normalize inside `TelemetryClient`, not in the renderer**

Refactor `TelemetryClient.kt` so it keeps `StateFlow`s for the explicit messages and then synthesizes the existing app-facing normalized state:
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

Do the normalization in small helpers such as:

```kotlin
private fun applyControlTelemetry(control: CommaViewControlV2): Boolean
private fun applySceneTelemetry(scene: CommaViewSceneV2): Boolean
private fun applyStatusTelemetry(status: CommaViewStatusV2): Boolean
```

**Step 5: Replace the old review contract**

Turn `HudLiteCutoverSourceContractTest.kt` into a v2 guard that asserts:
- no legacy raw fallback symbols exist
- `TelemetryClient` no longer maps `0 -> commaViewHudLite`
- the source contains `commaViewControl`, `commaViewScene`, and `commaViewStatus`

**Step 6: Run the focused app tests**

Run:

```bash
cd /home/pear/CommaView
./gradlew test --tests 'com.commaview.app.connection.raw.CapnpRawTelemetryDecoderTest' \
               --tests 'com.commaview.app.connection.TelemetryClientRawCapnpDecodingTest' \
               --tests 'com.commaview.app.connection.TelemetryClientHudLiteDecodingTest' \
               --tests 'com.commaview.app.review.HudLiteCutoverSourceContractTest'
```

Expected: all PASS.

**Step 7: Commit**

```bash
cd /home/pear/CommaView
git add app/src/main/java/com/commaview/app/model/OnroadParityTelemetry.kt \
        app/src/main/java/com/commaview/app/connection/raw/RawTelemetryDecoder.kt \
        app/src/main/java/com/commaview/app/connection/TelemetryClient.kt \
        app/src/test/java/com/commaview/app/connection/RawV4TelemetryFixtures.kt \
        app/src/test/java/com/commaview/app/connection/raw/CapnpRawTelemetryDecoderTest.kt \
        app/src/test/java/com/commaview/app/connection/TelemetryClientRawCapnpDecodingTest.kt \
        app/src/test/java/com/commaview/app/connection/TelemetryClientHudLiteDecodingTest.kt \
        app/src/test/java/com/commaview/app/review/HudLiteCutoverSourceContractTest.kt
git commit -m "feat(app): decode and normalize direct v2 ui export"
```

### Task 6: Rename the app-facing repair/status UX to the new export terminology

**Files:**
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/device/CommaViewApi.kt`
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/viewmodel/DeviceListViewModel.kt`
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/ui/DeviceListScreen.kt`
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/MainActivity.kt`
- Modify: `/home/pear/CommaView/app/src/test/java/com/commaview/app/device/CommaViewApiTest.kt`
- Modify: `/home/pear/CommaView/app/src/test/java/com/commaview/app/viewmodel/DeviceListPresentationTest.kt`

**Step 1: Make API tests fail on stale HUD-lite endpoints**

Update `CommaViewApiTest.kt` so it expects:

```kotlin
assertEquals("/commaview/onroad-ui-export/status", capturedPath)
assertEquals("/commaview/onroad-ui-export/repair", capturedPath)
```

Run:

```bash
cd /home/pear/CommaView
./gradlew test --tests 'com.commaview.app.device.CommaViewApiTest'
```

Expected: FAIL until `CommaViewApi.kt` is updated.

**Step 2: Rename API models and calls**

In `CommaViewApi.kt`, replace `HudLiteRuntimeStatus` / `repairHudLite()` naming with `OnroadUiExportRuntimeStatus` / `repairOnroadUiExport()` (or an equally explicit direct-v2 name). Keep the JSON parser strict enough to reject missing `onroadUiExport` payloads.

**Step 3: Rename device-list presentation state**

Update `DeviceListViewModel.kt`, `DeviceListScreen.kt`, and `MainActivity.kt` so the UI no longer says `hud-lite`, `HUD export`, or `repairHudLite`. The presentation copy should match the new control-plane naming, for example:
- `Onroad UI export: repair needed`
- `Repair onroad UI export`

**Step 4: Run focused UI/viewmodel tests**

Run:

```bash
cd /home/pear/CommaView
./gradlew test --tests 'com.commaview.app.device.CommaViewApiTest' \
               --tests 'com.commaview.app.viewmodel.DeviceListPresentationTest'
```

Expected: both PASS.

**Step 5: Commit**

```bash
cd /home/pear/CommaView
git add app/src/main/java/com/commaview/app/device/CommaViewApi.kt \
        app/src/main/java/com/commaview/app/viewmodel/DeviceListViewModel.kt \
        app/src/main/java/com/commaview/app/ui/DeviceListScreen.kt \
        app/src/main/java/com/commaview/app/MainActivity.kt \
        app/src/test/java/com/commaview/app/device/CommaViewApiTest.kt \
        app/src/test/java/com/commaview/app/viewmodel/DeviceListPresentationTest.kt
git commit -m "feat(app): rename repair ui for direct v2 export"
```

### Task 7: Run end-to-end verification before tagging anything

**Files:**
- Verify only; no intended source changes

**Step 1: Run full targeted runtime verification**

Run:

```bash
cd /home/pear/commaviewd
bash comma4/tests/hud_lite_patch_contract_test.sh
bash comma4/tests/hud_lite_canary_applicability_test.sh
bash commaviewd/tests/control_mode_api_contract_test.sh
bash commaviewd/tests/raw_only_runtime_contract_test.sh
bash commaviewd/tests/hud_lite_ci_contract_test.sh
bash commaviewd/tests/unit_tests_pipeline_test.sh
```

Expected: every script prints `PASS:`.

**Step 2: Run full targeted app verification**

Run:

```bash
cd /home/pear/CommaView
./scripts/verify-schema-snapshot.sh
./scripts/verify-generated-bindings-reproducible.sh
./gradlew test --tests 'com.commaview.app.connection.raw.CapnpRawTelemetryDecoderTest' \
               --tests 'com.commaview.app.connection.TelemetryClientRawCapnpDecodingTest' \
               --tests 'com.commaview.app.connection.TelemetryClientHudLiteDecodingTest' \
               --tests 'com.commaview.app.review.HudLiteCutoverSourceContractTest' \
               --tests 'com.commaview.app.device.CommaViewApiTest' \
               --tests 'com.commaview.app.viewmodel.DeviceListPresentationTest' \
               --tests 'com.commaview.app.rendering.overlay.parity.OnroadParityStateAdapterTest' \
               --tests 'com.commaview.app.rendering.overlay.scene.OverlaySceneBuilderTest'
```

Expected: all PASS.

**Step 3: Smoke-check lockstep failure behavior**

With a deliberately stale test fixture or forced `exportVersion = 1`, prove the app rejects the packet and does not silently consume it. Then restore the correct version and rerun the focused decoder/client tests.

**Step 4: Capture release readiness**

Before any tag or push announcement, record:
- runtime patch helper names
- final service index mapping
- final endpoint names
- exact test commands that passed

If anything still says `hud-lite` in the user-visible or control-plane contract, fix it before release. Half-renamed migrations are how ghosts get into the machine.

---

Plan complete and saved to `docs/plans/2026-03-22-direct-v2-parity-cutover-implementation.md`.

Two execution options:

1. **Subagent-Driven (this session)** - dispatch a fresh subagent per task, review between tasks, fast iteration
2. **Parallel Session (separate)** - open a new session with `executing-plans`, batch execution with checkpoints
