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
- **Mode-aware supervisor**
  - On-road: openpilot-managed encoder + bridge (prod mode)
  - Off-road: `camerad` + `encoderd --stream` + bridge (`--dev`)

---

## Quick start (comma install)

Install/update CommaView on comma from GitHub:

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma-install/install.sh | ssh comma@<comma-ip> bash
```

Optional Tailscale Guardian (opt-in):

```bash
# auth key via env
COMMAVIEW_TAILSCALE_AUTHKEY="tskey-auth-..." \
  curl -fsSL https://raw.githubusercontent.com/RhynoTech/CommaView/master/comma-install/install.sh \
  | ssh comma@<comma-ip> 'bash -s -- --enable-tailscale'
```

Safety policy:
- Onroad (`IsOnroad=1`): guardian forces Tailscale down
- Offroad + enabled flag: guardian starts/reconnects Tailscale
- Installer consumes auth key once and does not persist raw key

Current pinned release in installer: **`v0.1.2-alpha`**

Release assets:
- https://github.com/RhynoTech/CommaView/releases/tag/v0.1.2-alpha

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
- `/data/commaview/logs/tailscale-guardian.log`
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

Current app version target: **`0.1.2-alpha`**

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
comma-install/build-release-bundle.sh v0.1.2-alpha
```

Outputs:
- `release/v0.1.2-alpha/commaview-comma4-v0.1.2-alpha.tar.gz`
- `release/v0.1.2-alpha/commaview-comma4-v0.1.2-alpha.tar.gz.sha256`

Then upload both files to GitHub Release `v0.1.2-alpha` (or next tag), and update `comma-install/install.sh` pinned tag as needed.

---


## Maintainers: selective public publish workflow

Canonical working repo: `/home/pear/CommaView`

Use local `public-master` (tracks `github/master`) and export only allowlisted files:

```bash
# dry-run
tools/publish-public.sh --source master --dry-run

# real publish (fast-forward, no force push)
tools/publish-public.sh --source master
```

Allowlisted paths:
- `README.md`
- `DISCLAIMER.md`
- `LICENSE`
- `comma-install/install.sh`
- `comma-install/api/commaview-api.py`
- `comma-install/tailscale/tailscalectl.sh`
- `comma-install/tailscale/install_tailscale_runtime.sh`
- `comma-install/runtime/commaview-supervisor.sh`
- `comma-install/runtime/start.sh`
- `comma-install/runtime/stop.sh`
- `comma-install/runtime/uninstall.sh`
- `comma-install/runtime/upgrade.sh`
- `bridge/cpp/build-ubuntu.sh`
- `bridge/cpp/commaview-bridge.cc`
- `bridge/cpp/reproducible-build.sh`
- `bridge/cpp/run-unit-tests.sh`
- `bridge/cpp/run-verification.sh`
- `bridge/cpp/include/commaview/net/framing.h`
- `bridge/cpp/include/commaview/net/socket.h`
- `bridge/cpp/include/commaview/control/policy.h`
- `bridge/cpp/include/commaview/video/router.h`
- `bridge/cpp/include/commaview/telemetry/json_builder.h`
- `bridge/cpp/src/net/framing.cpp`
- `bridge/cpp/src/net/socket.cpp`
- `bridge/cpp/src/control/policy.cpp`
- `bridge/cpp/src/video/router.cpp`
- `bridge/cpp/src/telemetry/json_builder.cpp`
- `bridge/cpp/tests/reproducible_build_test.sh`
- `bridge/cpp/tests/unit_tests_pipeline_test.sh`
- `bridge/cpp/tests/test_net_framing.cpp`
- `bridge/cpp/tests/test_control_policy.cpp`
- `bridge/cpp/tests/test_telemetry_json.cpp`
- `docs/plans/2026-03-06-bridge-modularization-two-phase-implementation.md`
- `docs/reports/2026-03-06-bridge-modularization-baseline.md`
- `docs/reports/2026-03-06-bridge-phase1-parity.md`
- `docs/reports/2026-03-06-bridge-phase2-reproducible-build.md`
- `docs/reports/2026-03-06-bridge-phase2-tests-pipeline.md`

---

## License

See `LICENSE` (All Rights Reserved).
