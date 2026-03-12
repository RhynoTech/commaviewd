# commaviewd (Comma Runtime)

> ⚠️ Unofficial project. Community-maintained and not affiliated with or endorsed by comma.ai.

`commaviewd` is the comma-side C++ runtime for CommaView, plus comma4 install/release tooling.

## At a glance

- Runtime binary with explicit modes:
  - `commaviewd bridge` (video + telemetry streaming)
  - `commaviewd control` (local control API + tailscale policy)
- Installer lifecycle scripts for comma4 (`comma4/`)
- CI + canary coverage against upstream openpilot/sunnypilot branches

## Repository layout

- `commaviewd/` — runtime source, tests, verification scripts
- `comma4/` — install/start/stop/upgrade/uninstall scripts + version pin
- `tools/release/` — release bundle builder
- `.github/workflows/` — CI/release/canary workflows

## Install on comma4

Install/update:

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/commaviewd/master/comma4/install.sh | ssh comma@<comma-ip> bash
```

Optional tailscale onboarding:

```bash
COMMAVIEW_TAILSCALE_AUTHKEY="tskey-auth-..." \
  curl -fsSL https://raw.githubusercontent.com/RhynoTech/commaviewd/master/comma4/install.sh \
  | ssh comma@<comma-ip> 'bash -s -- --enable-tailscale'
```

Upgrade / uninstall:

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/commaviewd/master/comma4/upgrade.sh | ssh comma@<comma-ip> bash
ssh comma@<comma-ip> 'bash /data/commaview/uninstall.sh'
```

## Verification + release

Run full runtime verification pipeline:

```bash
commaviewd/scripts/run-verification.sh
```

Coverage includes:

- upstream interface guard
- reproducible build check
- binary contract check
- unit tests
- release-bundle smoke packaging

Build release bundle assets:

```bash
tools/release/comma4-build-bundle.sh <tag>
```

## CI targets

Main CI matrix:

- `commaai/openpilot@release-mici-staging`
- `sunnypilot/sunnypilot@staging`

Daily canaries:

- openpilot: `release-mici-staging`, `nightly`
- sunnypilot: `staging`, `dev`

## Related app repository

- **Android app (private):** `RhynoTech/CommaView`

## Safety / legal

- Read `DISCLAIMER.md` before use.
- License: `LICENSE` (All Rights Reserved).
