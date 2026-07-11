# CommaView Android app release audit for TIZI/TICI runtime testing

Date: 2026-07-11

## Scope

This audit covers the separate Android app repository, `RhynoTech/CommaView`, because the runtime release branch was reset back to the last stable TCP-era runtime line for comma 3/3X hook testing. The question is whether the Android app `master` has the same kind of post-release UDP transport work that was moved out of the runtime release path, and what app-side changes are needed to test the restored TIZI/TICI runtime.

## Repository and release baseline

The Android app repository was cloned from `https://github.com/RhynoTech/CommaView.git` into `/tmp/CommaView-audit` for inspection.

The latest app release tag found in that repo is `app-v0.0.156-alpha`:

- tag commit: `870982a8 Prepare app v0.0.156 release`
- release metadata at the tag:
  - `APP_VERSION_CODE=156`
  - `APP_VERSION_NAME=0.0.156-alpha`
  - `COMPATIBLE_RUNTIME_TAG=v0.0.53-alpha`

Current app `master` is:

- head commit: `86073615 Keep UDP preview tolerant after non-keyframe loss`
- release metadata on `master`:
  - `APP_VERSION_CODE=158`
  - `APP_VERSION_NAME=0.0.158-alpha`
  - `COMPATIBLE_RUNTIME_TAG=v0.0.55-alpha`

There are 108 commits from `app-v0.0.156-alpha` to current app `master`.

## Post-release app change classification

The post-release Android app changes are not limited to a small compatibility patch. They are a large app-side transport and recording line.

Major groups after `app-v0.0.156-alpha`:

1. HEVC recording quality presets and export controls.
2. Shared onroad renderer extraction for live preview and recording export parity.
3. App update/runtime update UI work.
4. UDP video packet protocol, preview reassembly, camera stream receiving, binary UDP control, packet-loss handling, and diagnostics.
5. UDP telemetry snapshot receiving for live overlay/HUD telemetry.
6. UDP recording repair, sparse repair, raw index metadata, and repair sidecars.
7. Tooling/session-hook and test robustness work.

The most important release-planning point is that app `master` removed the old chunked TCP camera receiver path and now contains UDP-specific streaming classes such as `UdpCameraStreamReceiver`, `UdpPreviewFrameReassembler`, `UdpTelemetrySnapshotReceiver`, and `UdpVideoPacketProtocol`. At the `app-v0.0.156-alpha` tag, the streaming package still contains the TCP-era `HevcStreamReceiver` and `VideoChunkProtocol` classes.

## App branch recommendation

The Android app `master` should be treated the same way the runtime UDP line was treated: preserve it on a dedicated feature branch before preparing a stable TIZI/TICI device-test app release.

Recommended branch:

```text
feature/udp-transport-app -> 86073615 Keep UDP preview tolerant after non-keyframe loss
```

Rationale:

- App `master` is already paired with the newer UDP runtime line through `COMPATIBLE_RUNTIME_TAG=v0.0.55-alpha`.
- The restored runtime `master` for TIZI/TICI testing is intentionally a TCP-era stable runtime line plus hook-installation support, not the UDP transport rewrite.
- Testing the restored runtime with current app `master` would mix a TCP-era runtime with a UDP-era app and would not be a clean comma 3/3X validation.

## Stable app line for comma 3/3X runtime testing

For the immediate device-testing release, use `app-v0.0.156-alpha` as the app base unless a newer pre-UDP app release is identified. That tag is still on the TCP-era runtime interface and is pinned to `COMPATIBLE_RUNTIME_TAG=v0.0.53-alpha`.

The app-side work needed for comma 3/3X should be intentionally small:

1. Bump app release metadata to a new alpha version.
2. Update `COMPATIBLE_RUNTIME_TAG` to the new restored runtime tag that contains TIZI/TICI hook installation support.
3. Keep the app on the existing TCP control/video/telemetry interface for this test release.
4. Do not cherry-pick the UDP receiver, UDP telemetry snapshot, or recording repair line into the stable comma 3/3X test app.
5. Verify that the UI does not label support as comma four-only when pairing/installing the runtime.
6. Confirm the app can install or select the new runtime tag and then connect to a MICI device and a TIZI/TICI device.

## Expected compatibility model

The restored runtime work is designed to keep the app-facing telemetry and video contract stable while changing how the runtime installs the upstream UI hook on the comma device. Therefore the app should not need TIZI-specific rendering or transport changes for the first device test.

Expected behavior:

- MICI and TIZI/TICI both feed the same sidecar/runtime socket contract.
- The runtime installer chooses the upstream hook target by device/platform.
- The Android app connects to the runtime the same way it did for the previous stable release.
- Device differences should appear in runtime patch status and diagnostics, not as a separate app transport.

## Audit commands

Commands used for this audit:

```bash
git clone --filter=blob:none https://github.com/RhynoTech/CommaView.git /tmp/CommaView-audit
git fetch --tags --force origin '+refs/heads/*:refs/remotes/origin/*'
git tag --sort=-v:refname | head -30
git log -1 --format='%h %cs %s' app-v0.0.156-alpha^{}
git show app-v0.0.156-alpha:release.properties
git show origin/master:release.properties
git rev-list --count app-v0.0.156-alpha..origin/master
git log --reverse --oneline app-v0.0.156-alpha..origin/master
git ls-tree -r --name-only app-v0.0.156-alpha app/src/main/java/com/commaview/app/streaming
git ls-tree -r --name-only origin/master app/src/main/java/com/commaview/app/streaming
git diff --stat app-v0.0.156-alpha..origin/master
```

## Open questions before cutting the app test release

1. Confirm whether the intended stable app baseline should be exactly `app-v0.0.156-alpha`, or whether there is a later app release candidate branch that is still pre-UDP.
2. Decide the new app version number to pair with the restored runtime release.
3. Decide whether to create and push `feature/udp-transport-app` before resetting/rebuilding app `master` for the stable test release.
4. Confirm the new runtime tag name so `COMPATIBLE_RUNTIME_TAG` can be updated in the app release metadata.

## Install and UI guard audit for comma 3/3X

The stable Android app line does not appear to have an app-side hardware guard that blocks comma 3 or comma 3X from installing the runtime.

Findings from `app-v0.0.156-alpha`:

- The setup flow calls `SshInstaller.install(host, script)` with `InstallScriptSource.remoteInstallScript(...)` after a generic SSH connectivity check. It does not branch on `deviceModel`, `mici`, `tici`, or `tizi` before installing.
- `InstallScriptSource.remoteInstallScript(...)` pipes the configured runtime install URL into `bash --tag <runtime-tag>` with optional `--force-offroad`. It does not pass or enforce a comma-four-only platform value.
- `RuntimeReleaseConfig.INSTALL_SCRIPT_PATH` is still `comma4/install.sh`, so the main compatibility issue is naming/copy and raw installer path, not an Android-side hardware block.
- The device identity parser already accepts a generic `deviceModel` field from runtime `/commaview/version` or discovery responses.
- Device card avatar labels already recognize `comma 3X`/`comma3x` as `C3X` and `comma 3`/`comma3` as `C3`, so the app is not strictly comma-four-only in device-list presentation.

Recommended app-side changes before the comma 3/3X test app release:

1. Keep installation permissive for any paired comma device and let the runtime installer enforce actual support.
2. Update visible copy from `comma4 install/upgrade scripts` or comma-four-specific language to `comma device` / `comma runtime` where user-facing.
3. Consider renaming the runtime install path only when the runtime release actually provides a stable `comma/install.sh` path. For the immediate restored-runtime release, the app can keep `comma4/install.sh` if the runtime tag still ships that path.
4. Add a regression test that a `deviceModel` like `comma 3X`, `comma tizi`, or `comma tici` does not suppress setup/reinstall actions.
5. Once the runtime exposes `uiPlatform` in patch status, optionally surface it in diagnostics/settings for support, but do not make it a blocker for install.

Conclusion: no install blocker was found in the stable Android app path. We should still clean up wording and add tests so future app changes do not accidentally reintroduce a comma-four-only assumption.
