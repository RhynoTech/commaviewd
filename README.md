# CommaView

> ⚠️ **Unofficial project:** CommaView is community-maintained and is **not** affiliated with, endorsed by, or supported by comma.ai.

CommaView provides a live camera view + telemetry HUD from a comma device to Android devices over LAN.

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
- **Runtime supervisor**
  - Single supervisor process for bridge watchdog + tailscale policy
  - openpilot manager owns camerad/encoderd lifecycle

---

## Quick start (comma install)

Install/update CommaView on comma from GitHub:

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma-install/install.sh | ssh comma@<comma-ip> bash
```

Optional Tailscale access (opt-in):

```bash
# auth key via env
COMMAVIEW_TAILSCALE_AUTHKEY="tskey-auth-..." \
  curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma-install/install.sh \
  | ssh comma@<comma-ip> 'bash -s -- --enable-tailscale'
```

Safety policy:
- Onroad (`IsOnroad=1`): supervisor policy forces Tailscale down
- Offroad + enabled flag: supervisor policy ensures Tailscale is up
- Installer consumes auth key once and does not persist raw key

Current pinned release in installer: **`v0.1.3-alpha`**

Release assets:
- https://github.com/RhynoTech/CommaView/releases/tag/v0.1.3-alpha

APK distribution remains private (not published in GitHub releases).

### Upgrade existing install (offroad only)

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma-install/runtime/upgrade.sh | ssh comma@<comma-ip> bash
```

The upgrader hard-blocks when `IsOnroad=1`. Park first, then run upgrade.

### Uninstall

```bash
ssh comma@<comma-ip> 'bash /data/commaview/uninstall.sh'
```

---

### Tailscale controls and troubleshooting

On comma device:

```bash
bash /data/commaview/tailscale/tailscalectl.sh status --json
bash /data/commaview/tailscale/tailscalectl.sh enable
bash /data/commaview/tailscale/tailscalectl.sh disable
```

Logs:
- `/data/commaview/logs/supervisor.log`
- `/data/commaview/logs/tailscale-install.log`

Rollback:
- disable remote access: `bash /data/commaview/tailscale/tailscalectl.sh disable`
- full removal: `bash /data/commaview/uninstall.sh`

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

Current app version target: **`0.1.3-alpha`**

### First-run onboarding (install-first redesign)

Remote access settings now include:
- Enable/disable tailscale toggle
- Submit one-time auth key (for joining your tailnet)
- One-tap upgrade action + manual upgrade command fallback


On fresh install, CommaView now uses an install-first setup flow:

1. Welcome + safety acknowledgment
2. Install method choice:
   - **Guided** (default, recommended)
   - **Manual / Advanced** (one-command path)
3. Service verification
4. Discovery + connect
   - Auto-discovery when available
   - **Add by IP** fallback always available
5. Live handoff

Setup progress is persisted, so if the app is restarted during setup it should resume at the nearest valid step instead of resetting to the beginning.

---

## Maintainers: cutting a new bridge release bundle

Build release bundle + checksums:

```bash
comma-install/build-release-bundle.sh v0.1.3-alpha
```

Outputs:
- `release/v0.1.3-alpha/commaview-comma4-v0.1.3-alpha.tar.gz`
- `release/v0.1.3-alpha/commaview-comma4-v0.1.3-alpha.tar.gz.sha256`

Then upload both files to GitHub Release `v0.1.3-alpha` (or next tag), and update `comma-install/install.sh` pinned tag as needed.

---

## License

See `LICENSE` (All Rights Reserved).
