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
