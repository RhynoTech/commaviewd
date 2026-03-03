# CommaView

> ⚠️ **Unofficial project:** CommaView is community-maintained and is **not** affiliated with, endorsed by, or supported by comma.ai.

CommaView provides a live camera view + telemetry HUD from a comma device to Android devices over LAN.


> 🚧 **Source availability:** Public source code is currently limited while the project is still in active alpha iteration.
>
> For now, this repo primarily publishes installer + release artifacts. Full source publication will happen once APIs/install flow stabilize.

---

## What it does

- Streams camera video to Android (road/wide + optional driver PiP)
- Shows telemetry HUD (speed, engagement, alerts, device state)
- Supports per-client suppression of video transmission (telemetry-only mode)

## Current architecture

- **Android app** (`app/`) — HEVC receiver + HUD rendering
- **C++ bridge** on comma (`/data/commaview/commaview-bridge`)
  - Video frames over TCP framing (`MSG_VIDEO`)
  - Telemetry JSON over same stream (`MSG_META`)
  - In-band control (`MSG_CONTROL`) for per-client suppression policy
- **Mode-aware supervisor**
  - On-road: openpilot-managed encoder + bridge (prod mode)
  - Off-road: `camerad` + `encoderd --stream` + bridge (`--dev`)

---

## Quick start (comma install)

Install/update CommaView on comma from GitHub:

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma-install/install.sh | ssh comma@<comma-ip> bash
```

Current pinned release in installer: **`v0.1.1-alpha`**

Release assets:
- https://github.com/RhynoTech/CommaView/releases/tag/v0.1.1-alpha

### Uninstall

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma-install/uninstall.sh | ssh comma@<comma-ip> bash
```

---

## Android app

### Build

```bash
./gradlew :app:assembleDebug
```

### Install (example)

```bash
adb -s <device-id> install -r app/build/outputs/apk/debug/app-debug.apk
```

Current app version target: **`0.1.1-alpha`**

---

## Maintainers: cutting a new bridge release bundle

Build release bundle + checksums:

```bash
comma-install/build-release-bundle.sh v0.1.1-alpha
```

Outputs:
- `release/v0.1.1-alpha/commaview-comma4-v0.1.1-alpha.tar.gz`
- `release/v0.1.1-alpha/commaview-comma4-v0.1.1-alpha.tar.gz.sha256`

Then upload both files to GitHub Release `v0.1.1-alpha` (or next tag), and update `comma-install/install.sh` pinned tag as needed.

---

## License

TBD
