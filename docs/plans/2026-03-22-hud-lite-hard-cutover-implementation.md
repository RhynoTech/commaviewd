# HUD-lite Hard Cutover Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace all direct subscription-based telemetry in `commaviewd` with a single UI-owned HUD-lite Cap'n Proto export, delivered through a small install-time openpilot/sunnypilot patchset, with strict no-fallback behavior when the export is missing.

**Architecture:** `commaviewd` stops subscribing to raw telemetry services (`carState`, `controlsState`, `deviceState`, etc.) and instead consumes one HUD-lite service published from the on-device UI process using values already present in the UI's shared `SubMaster`. `commaviewd` keeps a single binary but splits video and HUD-lite telemetry into separate internal loops/threads. Patch lifecycle is managed by install/upgrade verification plus offroad auto-repair and an explicit app-side repair action.

**Tech Stack:** C++ (`commaviewd`), install/upgrade shell scripts, patch files for openpilot/sunnypilot UI code, Android Kotlin app (`/home/pear/CommaView`), Cap'n Proto schema, comma4 runtime verification.

---

### Task 1: Add failing patch-lifecycle and hard-cutover contract tests in `commaviewd`

**Files:**
- Create: `comma4/tests/hud_lite_patch_contract_test.sh`
- Modify: `commaviewd/tests/unit_tests_pipeline_test.sh`
- Create: `comma4/patches/openpilot/0001-hud-lite-export.patch`
- Create: `comma4/patches/sunnypilot/0001-hud-lite-export.patch`
- Optional helper target: `comma4/install.sh`, `comma4/upgrade.sh`, `commaviewd/src/control_mode.cpp`

**Step 1: Write the failing tests**
Add a shell contract test that asserts the repo now has explicit HUD-lite patch assets and verification hooks, for example:
- openpilot patch file exists
- sunnypilot patch file exists
- install/upgrade scripts reference HUD-lite patch apply/verify logic
- control API or runtime status path has a HUD-lite health concept
- raw telemetry runtime config defaults are no longer treated as the primary telemetry mechanism

Example assertions:

```bash
[[ -f "$ROOT/comma4/patches/openpilot/0001-hud-lite-export.patch" ]] || fail "missing openpilot HUD-lite patch"
[[ -f "$ROOT/comma4/patches/sunnypilot/0001-hud-lite-export.patch" ]] || fail "missing sunnypilot HUD-lite patch"
grep -Fq "hud-lite" "$ROOT/comma4/install.sh" || fail "install path should manage HUD-lite patch lifecycle"
grep -Fq "hud-lite" "$ROOT/comma4/upgrade.sh" || fail "upgrade path should manage HUD-lite patch lifecycle"
```

**Step 2: Run tests to verify they fail**

Run:
- `cd /home/pear/commaviewd && comma4/tests/hud_lite_patch_contract_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/unit_tests_pipeline_test.sh`

Expected: failure because HUD-lite patch assets and lifecycle hooks do not exist yet.

**Step 3: Write minimal test scaffolding only**
- Create empty-but-tracked patch file placeholders if needed.
- Wire the new contract test into `unit_tests_pipeline_test.sh`.
- Do not implement installer/runtime behavior yet.

**Step 4: Re-run tests to confirm they still fail for the intended reason**

Run the same commands again.

Expected: failure remains targeted at missing HUD-lite lifecycle implementation, not missing test file wiring.

**Step 5: Commit**

```bash
cd /home/pear/commaviewd
git add comma4/tests/hud_lite_patch_contract_test.sh commaviewd/tests/unit_tests_pipeline_test.sh comma4/patches/openpilot/0001-hud-lite-export.patch comma4/patches/sunnypilot/0001-hud-lite-export.patch
git commit -m "test(hud-lite): add patch lifecycle contract coverage"
```

### Task 2: Implement HUD-lite patch assets and patch lifecycle management in `commaviewd`

**Files:**
- Create: `comma4/patches/openpilot/0001-hud-lite-export.patch`
- Create: `comma4/patches/sunnypilot/0001-hud-lite-export.patch`
- Modify: `comma4/install.sh`
- Modify: `comma4/upgrade.sh`
- Modify: `commaviewd/src/control_mode.cpp`
- Optional create: `comma4/scripts/verify_hud_lite_patch.sh`
- Optional create: `comma4/scripts/apply_hud_lite_patch.sh`
- Test: `comma4/tests/hud_lite_patch_contract_test.sh`

**Step 1: Re-run the failing HUD-lite contract tests**

Run:
- `cd /home/pear/commaviewd && comma4/tests/hud_lite_patch_contract_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/unit_tests_pipeline_test.sh`

Expected: fail before production changes.

**Step 2: Implement minimal production behavior**
- Add tiny patch files that modify the target upstream UI files to:
  - add the HUD-lite Cap'n Proto service definition
  - publish HUD-lite from existing UI-owned shared `SubMaster` data
- Add install/upgrade logic that:
  - detects openpilot vs sunnypilot target
  - applies the corresponding patchset
  - records patch fingerprint/version
  - verifies service presence
- In `control_mode.cpp`, add HUD-lite health/repair status endpoints used by the app.
- Enforce the no-fallback rule: missing patch/export means telemetry disabled, not raw fallback.

Patch content should target existing UI-owned data only, not add new UI subscribers.

**Step 3: Run tests to verify they pass**

Run:
- `cd /home/pear/commaviewd && comma4/tests/hud_lite_patch_contract_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/unit_tests_pipeline_test.sh`

Expected: pass.

**Step 4: Run focused lifecycle verification**

Run at least:
- `cd /home/pear/commaviewd && python3 comma4/tests/commaview_api_runtime_debug_config_test.py`
- plus any new HUD-lite health/repair test you add

Expected: existing API contract coverage still passes; new HUD-lite lifecycle hooks are green.

**Step 5: Commit**

```bash
cd /home/pear/commaviewd
git add comma4/patches/openpilot/0001-hud-lite-export.patch comma4/patches/sunnypilot/0001-hud-lite-export.patch comma4/install.sh comma4/upgrade.sh commaviewd/src/control_mode.cpp comma4/scripts/verify_hud_lite_patch.sh comma4/scripts/apply_hud_lite_patch.sh
git commit -m "feat(hud-lite): manage UI export patch lifecycle"
```

### Task 3: Hard-cut `commaviewd` over to HUD-lite-only telemetry

**Files:**
- Modify: `commaviewd/src/bridge_runtime.cc`
- Modify: `commaviewd/src/runtime_debug_config.h`
- Modify: `commaviewd/src/telemetry_policy.h`
- Possibly modify: `commaviewd/src/json_builder.cpp` or framing helpers if service indexing changes
- Tests: `commaviewd/tests/runtime_debug_policy_contract_test.sh`, `commaviewd/tests/raw_only_runtime_contract_test.sh`, new/updated HUD-lite runtime contract tests

**Step 1: Add failing cutover tests**
Before changing production logic, update/add tests so they fail until the hard cutover exists. Assertions should encode:
- no direct raw telemetry subscriber matrix remains for overlay telemetry
- HUD-lite is the sole telemetry service consumed by bridge runtime
- direct raw telemetry fallback is absent
- video and telemetry loops are separated internally (or at least telemetry processing is not in the same raw subscription path)

**Step 2: Run tests to verify they fail**

Run the targeted runtime contract suite.

Expected: fail before bridge runtime cutover.

**Step 3: Implement minimal production cutover**
- Remove old direct raw telemetry subscription matrix for overlay telemetry.
- Add a single HUD-lite subscriber path in `bridge_runtime.cc`.
- Move telemetry handling into its own internal loop/thread while keeping the single `commaviewd` binary.
- Preserve video bridge path and control path.
- Delete or simplify runtime-debug service policy code that no longer applies to telemetry subscriptions.

**Step 4: Run tests to verify they pass**

Run:
- `cd /home/pear/commaviewd && commaviewd/tests/runtime_debug_policy_contract_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/raw_only_runtime_contract_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/unit_tests_pipeline_test.sh`

Expected: pass with HUD-lite-only model.

**Step 5: Commit**

```bash
cd /home/pear/commaviewd
git add commaviewd/src/bridge_runtime.cc commaviewd/src/runtime_debug_config.h commaviewd/src/telemetry_policy.h commaviewd/src/json_builder.cpp commaviewd/tests/runtime_debug_policy_contract_test.sh commaviewd/tests/raw_only_runtime_contract_test.sh
git commit -m "feat(runtime): hard-cut telemetry over to HUD-lite"
```

### Task 4: Update Android app to consume HUD-lite and surface repair state

**Files:**
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/connection/TelemetryClient.kt`
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/connection/raw/RawTelemetryDecoder.kt`
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/model/OnroadParityTelemetry.kt`
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/device/CommaViewApi.kt`
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/ui/DeviceListScreen.kt`
- Modify: `/home/pear/CommaView/app/src/main/java/com/commaview/app/viewmodel/DeviceListViewModel.kt`
- Create tests under: `/home/pear/CommaView/app/src/test/java/...`

**Step 1: Add failing app-side tests**
Add tests that assert:
- HUD-lite decode populates required overlay fields
- old raw per-service overlay dependencies are no longer required
- device card can show HUD export healthy / stale / repair-needed
- repair action is exposed through the viewmodel/API surface

**Step 2: Run tests to verify they fail**

Run the narrow app test targets you add.

Expected: fail before app changes.

**Step 3: Implement app cutover**
- Make telemetry client decode/use HUD-lite as the sole overlay telemetry source.
- Update parity models/adapters to consume the bundled HUD-lite payload.
- Add HUD-lite health/repair status fetch + repair action wiring.
- Surface `Repair HUD Export` on the device card when appropriate.

**Step 4: Run tests and build verification**

Run:
- `cd /home/pear/CommaView && ./gradlew testDebugUnitTest`
- `cd /home/pear/CommaView && ./gradlew assembleDebug`

If the full unit test target is too broad, run the exact narrower test classes you added, then `assembleDebug`.

**Step 5: Commit**

```bash
cd /home/pear/CommaView
git add app/src/main/java/com/commaview/app/connection/TelemetryClient.kt app/src/main/java/com/commaview/app/connection/raw/RawTelemetryDecoder.kt app/src/main/java/com/commaview/app/model/OnroadParityTelemetry.kt app/src/main/java/com/commaview/app/device/CommaViewApi.kt app/src/main/java/com/commaview/app/ui/DeviceListScreen.kt app/src/main/java/com/commaview/app/viewmodel/DeviceListViewModel.kt app/src/test/java
git commit -m "feat(app): consume HUD-lite telemetry and surface repair state"
```

### Task 5: Full integration verification, release sanity, and device validation

**Files:**
- Modify: none unless verification exposes defects
- Verify across: `/home/pear/commaviewd`, `/home/pear/CommaView`, comma4 device runtime

**Step 1: Run cross-repo verification**

Run:
- `cd /home/pear/commaviewd && commaviewd/tests/runtime_debug_policy_contract_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/raw_only_runtime_contract_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/unit_tests_pipeline_test.sh`
- `cd /home/pear/commaviewd && OP_ROOT=/home/pear/openpilot-src ./commaviewd/scripts/run-unit-tests.sh`
- `cd /home/pear/CommaView && ./gradlew testDebugUnitTest assembleDebug`

Expected: all pass.

**Step 2: Build runtime release artifact and install locally**

Run:
- `cd /home/pear/commaviewd && tools/release/comma4-build-bundle.sh <new-tag>`
- install/apply to comma4 test device using existing install/upgrade path

Expected: HUD-lite patch applies/validates on device, runtime version reflects the new tag, HUD-lite health endpoint reports healthy.

**Step 3: On-device behavioral verification**
- confirm `commaviewd` no longer creates raw telemetry subscribers
- confirm HUD-lite export exists and streams expected values
- verify offroad auto-repair works after simulating patch loss
- verify onroad repair is blocked and device card reports repair-needed
- verify video + telemetry on-road remains stable

**Step 4: Confirm clean repo states before release claims**

Run:
- `git -C /home/pear/commaviewd status --short --branch`
- `git -C /home/pear/CommaView status --short --branch`

Expected: only intentional changes or clean trees.

**Step 5: Push/release only after proof is clean**
- push `commaviewd` runtime changes and tag runtime release
- push `CommaView` app changes only when Rhyno explicitly wants the app side shipped
- include proof in the handoff: commits, test output, bundle path, device validation notes
