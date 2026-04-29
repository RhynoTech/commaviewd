# commaviewd (Comma Runtime)

> ⚠️ Unofficial project. Community-maintained and not affiliated with or endorsed by comma.ai.

`commaviewd` is the comma-side C++ runtime for CommaView, plus comma4 install, patch, verification, and release tooling.

## At a glance

- Runtime binary with explicit modes:
  - `commaviewd bridge` — video + telemetry streaming runtime.
  - `commaviewd control` — local HTTP control API for pairing, status, runtime debug, and patch repair.
- comma4 lifecycle scripts under `comma4/`.
- Build/release/verification scripts under `commaviewd/scripts/`, `tools/release/`, and `scripts/`.
- CI + canary coverage against upstream openpilot/sunnypilot branches.

## Repository layout

- `commaviewd/` — runtime source, tests, and verification scripts.
- `comma4/` — comma-device install/start/stop/uninstall scripts, runtime defaults, patch helpers, and version pin.
- `tools/release/` — release bundle builder.
- `scripts/` — host setup and upstream-canary helper scripts.
- `.github/workflows/` — CI, release, and canary workflows.

## Runtime CLI

```bash
commaviewd bridge [bridge flags]
commaviewd control [control flags]
commaviewd --help
```

| Mode | Flags | Purpose |
| --- | --- | --- |
| `bridge` | none currently | Starts video + telemetry bridge. Runtime debug behavior is configured through JSON/env files, not CLI flags. |
| `control` | `--port <port>` | Starts local control API. Defaults to the built-in API port when omitted. |
| `--help`, `-h`, `help` | n/a | Prints mode usage. |

Runtime env used by installed scripts:

| Env | Default | Purpose |
| --- | --- | --- |
| `COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS` | `/data/commaview/runtime-debug.defaults.json` | Default runtime-debug policy source. |
| `COMMAVIEWD_RUNTIME_DEBUG_CONFIG` | `/data/commaview/config/runtime-debug.json` | Persisted editable runtime-debug config. |
| `COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE` | `/data/commaview/run/runtime-debug-effective.json` | Effective config emitted by runtime. |
| `COMMAVIEWD_RUNTIME_STATS` | `/data/commaview/run/telemetry-stats.json` | Telemetry stats output. |
| `COMMAVIEWD_RESTART_REASON` | `startup` | Written into runtime status/logs. |
| `COMMAVIEWD_API_TOKEN` | unset | Direct control API bearer token override. |
| `COMMAVIEWD_API_TOKEN_FILE` | `/data/commaview/api/auth.token` when launched by `start.sh` | Control API token file. |
| `COMMAVIEWD_UI_EXPORT_SOCKET` | `/data/commaview/run/ui-export.sock` in patch helper code | UI export Unix socket path override. |
| `COMMAVIEW_PARAMS_DIR` | platform default | Params directory override used by runtime JSON helpers. |

## Install/update on comma4

Recommended install/update from a workstation:

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/commaviewd/master/comma4/install.sh \
  | ssh comma@<comma-ip> bash
```

Install/update a specific release:

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/commaviewd/master/comma4/install.sh \
  | ssh comma@<comma-ip> bash -s -- --tag vX.Y.Z
```

Reinstall the currently installed release:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/install.sh --current'
```

Force offroad before install/update:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/install.sh --force-offroad'
```

`comma4/install.sh` flags and release env:

| Flag/env | Purpose |
| --- | --- |
| `--tag <release-tag>` | Install/update to a specific GitHub release tag. |
| `--current` | Reinstall the currently installed release from `/data/commaview/version.env`. Without this, installer resolves latest by default. |
| `--force-offroad` | Set `OffroadMode` and wait for a real offroad transition before changing files. |
| `-h`, `--help` | Print installer usage. |
| `COMMAVIEWD_RELEASE_REPO` | Override release repo; default `RhynoTech/commaviewd`. |
| `COMMAVIEWD_RELEASE_TAG` | Override resolved release tag. |
| `COMMAVIEWD_DEFAULT_TAG` | Fallback default tag before querying latest. |
| `COMMAVIEWD_INSTALLER_REF` | Pin companion scripts/patches to a ref; defaults to resolved release tag. |
| `COMMAVIEWD_ASSET_NAME` | Override release asset filename. |
| `COMMAVIEWD_BASE_URL` | Override release asset base URL. |
| `COMMAVIEWD_INSTALLER_RAW_BASE` | Override raw companion file base URL. |
| `COMMAVIEWD_VERSION` | Override displayed/stamped version. |

Installer safety behavior:

- Stages and validates the release bundle before stopping the live runtime.
- Refreshes companion scripts/patches from the resolved release instead of trusting stale installed files.
- Backs up managed install files before mutating `/data/commaview` and restores them if install fails mid-update.
- Clears stale runtime/patch state during install.
- Applies the onroad UI export patch through the patch helper; unsafe patch repair is not automatic.

## comma4 lifecycle scripts

| Script | Usage | Notes |
| --- | --- | --- |
| `comma4/start.sh` | `bash /data/commaview/start.sh` | Verifies/repairs the UI export patch when safe, consumes deferred UI restart marker, stops stale processes, then starts `commaviewd bridge` and `commaviewd control`. No CLI flags. Uses runtime env listed above. |
| `comma4/stop.sh` | `bash /data/commaview/stop.sh` | Stops pidfile-tracked bridge/control processes and cleans stray `/data/commaview/commaviewd` processes. No flags. |
| `comma4/uninstall.sh` | `bash /data/commaview/uninstall.sh` | Stops runtime, removes `/data/continue.sh` boot hook, deletes `/data/commaview`. No flags. |

Uninstall from workstation:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/uninstall.sh'
```

## Onroad UI export patch scripts

These scripts manage the direct v2 socket export patch in upstream openpilot/sunnypilot. The patch is additive, but it touches live upstream files, so default repair behavior is conservative.

| Script | Usage | Flags/env |
| --- | --- | --- |
| `comma4/scripts/verify_onroad_ui_export_patch.sh` | `bash /data/commaview/scripts/verify_onroad_ui_export_patch.sh [--json]` | `--json` prints machine-readable status. `COMMAVIEWD_INSTALL_DIR` overrides `/data/commaview`; `COMMAVIEWD_OP_ROOT` overrides `/data/openpilot`. |
| `comma4/scripts/apply_onroad_ui_export_patch.sh` | `bash /data/commaview/scripts/apply_onroad_ui_export_patch.sh [--force-offroad] [--force-repair]` | `--force-offroad` waits for offroad before patching. `--force-repair` is the only destructive repair path; it backs up targets before reset/reapply. `COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART=1` suppresses UI restart/marker behavior. |

Patch safety rules:

- Normal apply first verifies whether the patch is already applied or applies cleanly.
- If upstream changed and the patch no longer applies cleanly, the helper exits instead of resetting files.
- If target files are dirty, the helper exits instead of modifying them.
- `--force-repair` is explicit, offroad-gated through the existing flow, and backs up files under `/data/commaview/backups/onroad-ui-export/<timestamp>` before resetting/reapplying.

## Build scripts

| Script | Usage | Flags/env |
| --- | --- | --- |
| `scripts/install-commaviewd-toolchain.sh` | `bash scripts/install-commaviewd-toolchain.sh` | Installs host + arm64 build dependencies via `sudo apt`. Mutates apt source config and adds arm64 architecture; use only on a build host/runner. No flags. Emits `ARM_CAPNP_SO` and `ARM_KJ_SO`; writes GitHub outputs when `GITHUB_OUTPUT` is set. |
| `commaviewd/scripts/build-ubuntu.sh` | `OP_ROOT=/path/to/openpilot-src commaviewd/scripts/build-ubuntu.sh` | Builds host and aarch64 binaries into `DIST_DIR` (`dist/` by default). Env: `OP_ROOT`, `DIST_DIR`, `HOST_CXX`, `CXX`, `CROSS_CXX`, `COMMAVIEWD_SKIP_ARM=1`, `ARM_CAPNP_SO`, `ARM_KJ_SO`, `PATCHED_MSGQ_LOCAL`. No CLI flags. |
| `tools/release/comma4-build-bundle.sh` | `tools/release/comma4-build-bundle.sh [--skip-build] [<tag>]` | Builds/stages release bundle under `release/<tag>/`. `--skip-build` uses existing `DIST_DIR` artifacts. `<tag>` overrides `comma4/version.env` `RELEASE_TAG`. Env: `DIST_DIR`. |

## Verification scripts

| Script | Usage | Flags/env |
| --- | --- | --- |
| `commaviewd/scripts/run-verification.sh` | `OP_ROOT=/path/to/openpilot-src commaviewd/scripts/run-verification.sh` | Full verification pipeline: upstream interface guard, reproducible build, binary contract check, unit tests, release smoke bundle. Env: `OP_ROOT`, `DIST_DIR`, `RELEASE_SMOKE_TAG`. |
| `commaviewd/scripts/upstream-interface-guard.sh` | `OP_ROOT=/path/to/openpilot-src commaviewd/scripts/upstream-interface-guard.sh [--manifest <path>]` | Checks expected upstream schemas/services/patch applicability. Writes manifest to `DIST_DIR` by default. |
| `commaviewd/scripts/reproducible-build.sh` | `OP_ROOT=/path/to/openpilot-src commaviewd/scripts/reproducible-build.sh [--manifest <path>]` | Builds twice with fixed `SOURCE_DATE_EPOCH` and compares host/aarch64 digests. |
| `commaviewd/scripts/binary-contract-check.sh` | `DIST_DIR=/path/to/dist commaviewd/scripts/binary-contract-check.sh [--manifest <path>]` | Validates binary architecture, deps, runpath, size, and bundled runtime libraries. |
| `commaviewd/scripts/run-unit-tests.sh` | `OP_ROOT=/path/to/openpilot-src commaviewd/scripts/run-unit-tests.sh` | Builds runtime and compiles/runs C++ unit tests. Env: `OP_ROOT`, compiler env inherited by build script. |
| `scripts/verify-telemetry-hardening.sh` | `bash scripts/verify-telemetry-hardening.sh` | Grep-based guard that raw-only telemetry hardening remains in place and old dev/debug flags/env are absent. No flags. |

## Canary/upstream helper

```bash
scripts/sync-canary-upstream.sh <openpilot|sunnypilot> <ref> [dest-root]
```

Supported refs:

- `openpilot`: `release-mici-staging`, `nightly`
- `sunnypilot`: `staging`, `dev`

Default destination is `~/.cache/commaviewd-canary/<upstream>-<ref>/openpilot-src`. The script resolves the current ref SHA, force-checks out that SHA, initializes submodules, and writes `source.env` metadata.

## Test/contract scripts

These are mostly CI-facing but useful for targeted local checks.

| Script | Purpose |
| --- | --- |
| `comma4/tests/onroad_ui_export_patch_contract_test.sh` | Static contract check for the socket UI export patch. |
| `comma4/tests/onroad_ui_export_canary_applicability_test.sh` | Applies/verifies patch against real openpilot/sunnypilot canary refs. |
| `commaviewd/tests/control_mode_api_contract_test.sh` | Verifies control API routes/contract are present. |
| `commaviewd/tests/local_discovery_contract_test.sh` | Verifies local discovery responder contract. |
| `commaviewd/tests/onroad_ui_export_ci_contract_test.sh` | Ensures workflows align to direct v2 validation. |
| `commaviewd/tests/raw_only_runtime_contract_test.sh` | Guards raw-only runtime behavior. |
| `commaviewd/tests/reproducible_build_test.sh` | Guards reproducible build script behavior. |
| `commaviewd/tests/runtime_debug_policy_contract_test.sh` | Guards runtime-debug config/policy behavior. |
| `commaviewd/tests/timestamped_video_runtime_contract_test.sh` | Guards timestamped video runtime behavior. |
| `commaviewd/tests/unit_tests_pipeline_test.sh` | Guards unit-test pipeline script presence/help behavior. |

Python contract tests:

```bash
python3 -m pytest comma4/tests -q
```

## Standard local verification

```bash
python3 -m pytest comma4/tests -q
bash comma4/tests/onroad_ui_export_patch_contract_test.sh
bash comma4/tests/onroad_ui_export_canary_applicability_test.sh
bash commaviewd/tests/onroad_ui_export_ci_contract_test.sh
bash commaviewd/tests/runtime_debug_policy_contract_test.sh
bash commaviewd/tests/raw_only_runtime_contract_test.sh
bash commaviewd/tests/timestamped_video_runtime_contract_test.sh
bash commaviewd/tests/control_mode_api_contract_test.sh
bash commaviewd/tests/local_discovery_contract_test.sh
bash commaviewd/tests/unit_tests_pipeline_test.sh
```

Full runtime verification:

```bash
commaviewd/scripts/run-verification.sh
```

## Release flow

1. Update `comma4/version.env` with the target runtime release tag.
2. Run verification:

   ```bash
   commaviewd/scripts/run-verification.sh
   ```

3. Build release bundle:

   ```bash
   tools/release/comma4-build-bundle.sh <tag>
   ```

4. Push `master`, then tag with runtime format `v*`.
5. Confirm GitHub Actions release publishes:
   - `commaview-comma4-<tag>.tar.gz`
   - `commaview-comma4-<tag>.tar.gz.sha256`

## CI targets

Main CI matrix:

- `commaai/openpilot@release-mici-staging`
- `sunnypilot/sunnypilot@staging`

Daily canaries:

- openpilot: `release-mici-staging`, `nightly`
- sunnypilot: `staging`, `dev`

## Program plans and telemetry references

- `commaviewd/docs/COM-55-onroad-ui-parity-program.md` — phased execution plan for comma4 onroad UI parity.
- `commaviewd/docs/ai/telemetry-raw-only-readme.md` — short operator doc.
- `commaviewd/docs/ai/telemetry-raw-only-deep-dive.md` — deep technical doc.

## Related app repository

- **Android app (private):** `RhynoTech/CommaView`

## Safety / legal

- Read `DISCLAIMER.md` before use.
- License: `LICENSE` (All Rights Reserved).
