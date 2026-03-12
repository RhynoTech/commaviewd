# commaviewd

> ⚠️ **Unofficial project:** community-maintained, not affiliated with or endorsed by comma.ai.

`commaviewd` is the C++ comma-side runtime for CommaView, plus comma4 installer/release tooling.

## Repository scope

- `commaviewd/` — bridge/control runtime sources, tests, verification scripts
- `comma4/` — install / start / stop / upgrade / uninstall scripts
- `tools/release/` — release bundle build helper
- `.github/workflows/` — CI, release, and canary workflows

## Runtime model

Single binary with explicit modes:

- `commaviewd bridge` — stream lifecycle (video + telemetry)
- `commaviewd control` — local control API and tailscale policy handling

## Install on comma 4

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/commaviewd/master/comma4/install.sh | ssh comma@<comma-ip> bash
```

Optional tailscale onboarding:

```bash
COMMAVIEW_TAILSCALE_AUTHKEY="tskey-auth-..." \
  curl -fsSL https://raw.githubusercontent.com/RhynoTech/commaviewd/master/comma4/install.sh \
  | ssh comma@<comma-ip> 'bash -s -- --enable-tailscale'
```

## Upgrade / uninstall

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/commaviewd/master/comma4/upgrade.sh | ssh comma@<comma-ip> bash
ssh comma@<comma-ip> 'bash /data/commaview/uninstall.sh'
```

## Build / verify (maintainers)

```bash
commaviewd/scripts/run-verification.sh
```

This pipeline covers:

- upstream interface guard
- reproducible build check
- binary contract check
- unit tests
- release-bundle smoke packaging

## CI targets

Main CI matrix:

- `commaai/openpilot@release-mici-staging`
- `sunnypilot/sunnypilot@staging`

Daily canaries:

- openpilot: `release-mici-staging`, `nightly`
- sunnypilot: `staging`, `dev`

## Related Android app repo

The Android client now lives in a separate private repository:

- `RhynoTech/CommaView` (private)

## Safety / license

- Read `DISCLAIMER.md`.
- License: `LICENSE` (All Rights Reserved).
