# CommaView

> ⚠️ **Unofficial project:** CommaView is community-maintained and is **not** affiliated with, endorsed by, or supported by comma.ai.

CommaView streams camera video + telemetry from a comma device to Android clients on your network.

## What it does

- Streams HEVC video (road / wide, optional driver PiP)
- Overlays telemetry HUD (speed, engagement, alerts, device state)
- Supports per-client telemetry-only mode (video suppression)
- Provides offroad control APIs with Tailscale policy enforcement

## Repository layout

- `app/` — Android client (receiver + HUD)
- `commaviewd/` — C++ runtime (`bridge` + `control` modes)
- `comma4/` — installer, upgrade, start/stop, uninstall scripts + version pin
- `tools/release/` — release bundle tooling
- `docs/` — project and implementation notes

## Install on comma 4

Basic install/update:

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma4/install.sh | ssh comma@<comma-ip> bash
```

Install with optional Tailscale setup:

```bash
COMMAVIEW_TAILSCALE_AUTHKEY="tskey-auth-..." \
  curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma4/install.sh \
  | ssh comma@<comma-ip> 'bash -s -- --enable-tailscale'
```

Current pinned release is read from `comma4/version.env`.

## Upgrade / uninstall

Upgrade (offroad only):

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma4/upgrade.sh | ssh comma@<comma-ip> bash
```

Uninstall:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/uninstall.sh'
```

## Tailscale control API

Local status + toggles:

```bash
curl -sS http://127.0.0.1:5002/tailscale/status
curl -sS -X POST -H "X-CommaView-Token: <token>" http://127.0.0.1:5002/tailscale/enable
curl -sS -X POST -H "X-CommaView-Token: <token>" http://127.0.0.1:5002/tailscale/disable
```

Policy:

- Onroad (`IsOnroad=1`): Tailscale forced down
- Offroad + enabled flag: Tailscale brought up

Useful logs:

- `/data/commaview/logs/commaviewd-bridge.log`
- `/data/commaview/logs/commaviewd-control.log`
- `/data/commaview/logs/tailscale-install.log`

## Android app

Build debug APK:

```bash
./gradlew :app:assembleDebug
```

Install:

```bash
adb -s <device-id> install -r app/build/outputs/apk/debug/app-debug.apk
```

## Maintainer release flow

1. Update `comma4/version.env` (tag/version)
2. Build bundle + checksum:

```bash
tools/release/comma4-build-bundle.sh <tag>
```

Outputs:

- `release/<tag>/commaview-comma4-<tag>.tar.gz`
- `release/<tag>/commaview-comma4-<tag>.tar.gz.sha256`

3. Upload both assets to the matching GitHub Release tag.

## CI / canary coverage

Main CI (`commaviewd-ci`) runs against:

- `commaai/openpilot@release-mici-staging`
- `sunnypilot/sunnypilot@staging`

Canaries run daily and on demand:

- `commaviewd-canary-openpilot` (`release-mici-staging`, `nightly`)
- `commaviewd-canary-sunnypilot` (`staging`, `dev`)

Verification includes:

- upstream interface guard
- reproducible build check
- binary contract check
- unit tests
- release-bundle smoke packaging

## Safety and license

- Read `DISCLAIMER.md` before use.
- License: `LICENSE` (All Rights Reserved).
