# commaviewd (Comma Runtime)

> ⚠️ Unofficial project. Community-maintained and not affiliated with or endorsed by comma.ai.

`commaviewd` is the comma-side C++ runtime for CommaView, plus comma-device install, patch, verification, and release tooling.

## At a glance

- Runtime binary with explicit modes:
  - `commaviewd bridge` — video + telemetry streaming runtime.
  - `commaviewd control` — local HTTP control API for pairing, status, runtime debug, and patch repair.
- comma-device lifecycle scripts under `comma/`.
- Build/release/verification scripts under `commaviewd/scripts/`, `tools/release/`, and `scripts/`.
- CI + canary coverage against upstream openpilot/sunnypilot branches.

## Repository layout

- `commaviewd/` — runtime source, tests, and verification scripts.
- `comma/` — comma-device install/start/stop/uninstall scripts, runtime defaults, the onroad UI exporter and its transformer/patch scripts, and version pin.
- `comma4/install.sh` — back-compat installer shim that older app versions fetch; keep it.
- `tools/release/` — release bundle builder. `tools/bench/` — on-device benchmarks. `tools/contribute/` — route sanitizer for donated drives.
- `scripts/` — host toolchain setup, upstream-canary helper, telemetry-hardening guard, and release promotion script.
- `ci/` — pinned upstream refs. `docs/` — plans, reports, and per-release notes (`docs/release/<tag>-user-facing.md` becomes the GitHub release body).
- `.github/workflows/` — CI, release, canary, and device-test workflows.

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
| `COMMAVIEWD_RUNTIME_DEBUG_DEFAULTS` | `/data/commaview/runtime-debug.defaults.json` | Seed file `start.sh` copies into the config when that is missing or empty. The binary itself does not read it. |
| `COMMAVIEWD_RUNTIME_DEBUG_CONFIG` | `/data/commaview/config/runtime-debug.json` | Persisted editable runtime-debug config. |
| `COMMAVIEWD_RUNTIME_DEBUG_EFFECTIVE` | `/data/commaview/run/runtime-debug-effective.json` | Effective config emitted by runtime. |
| `COMMAVIEWD_RUNTIME_STATS` | `/data/commaview/run/telemetry-stats.json` | Telemetry stats output. |
| `COMMAVIEWD_RESTART_REASON` | `startup` | Written into runtime status/logs. |
| `COMMAVIEWD_API_TOKEN` | unset | Direct control API bearer token override. |
| `COMMAVIEWD_API_TOKEN_FILE` | `/data/commaview/api/auth.token` when launched by `start.sh` | Control API token file. |
| `COMMAVIEWD_UI_EXPORT_SOCKET` | `/data/commaview/run/ui-export.sock` | UI export Unix socket path override, read by both the bridge and the exporter. |
| `COMMAVIEWD_RECIPE_DIR` | `/data/commaview/recording-recipes` | Where the bridge writes and control mode serves recording recipes/snapshots. |
| `COMMAVIEWD_SOURCE_ARCHIVE_ROOT` | `/data/media/0/realdata` | Route segment archive served by the source-recording endpoints. |
| `COMMAVIEWD_CURRENT_ROUTE_FILE` | `/data/params/d/CurrentRoute` | Current route id used to name recordings. |
| `COMMAVIEW_VIDEO_SOURCE` | `full` | `full` (HEVC) or `livestream` (H.264); see below. |

## Install/update on comma-device

Recommended install/update from a workstation:

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/commaviewd/master/comma/install.sh \
  | ssh comma@<comma-ip> bash
```

Without `--tag`, the installer installs the runtime paired with the current CommaView app release, from `https://commaview.com/api/current-release`. A runtime that's tagged but not yet paired with an app release isn't installed this way. If that lookup fails, the installer falls back to the newest GitHub release.

Install/update a specific release:

```bash
curl -fsSL https://raw.githubusercontent.com/RhynoTech/commaviewd/master/comma/install.sh \
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

`comma/install.sh` flags and release env:

| Flag/env | Purpose |
| --- | --- |
| `--tag <release-tag>` | Install/update to a specific GitHub release tag. |
| `--current` | Reinstall the currently installed release from `/data/commaview/version.env`. |
| `--force-offroad` | Set `OffroadMode` and wait for a real offroad transition before changing files. |
| `-h`, `--help` | Print installer usage. |
| `COMMAVIEWD_RELEASE_REPO` | Override release repo; default `RhynoTech/commaviewd`. |
| `COMMAVIEWD_RELEASE_TAG` | Override resolved release tag. |
| `COMMAVIEWD_DEFAULT_TAG` | Tag to install when none is given, before any lookup. |
| `COMMAVIEWD_CURRENT_RELEASE_URL` | Override the current-release lookup used when no tag is given. Defaults to `https://commaview.com/api/current-release` for `RhynoTech/commaviewd`; other release repos skip it. |
| `COMMAVIEWD_RELEASES_API_URL` | Override the GitHub releases API used for the newest-release fallback. |
| `COMMAVIEWD_INSTALLER_REF` | Pin companion scripts to a ref; defaults to resolved release tag. |
| `COMMAVIEWD_ASSET_NAME` | Override release asset filename. |
| `COMMAVIEWD_BASE_URL` | Override release asset base URL. |
| `COMMAVIEWD_INSTALLER_RAW_BASE` | Override raw companion file base URL. |
| `COMMAVIEWD_VERSION` | Override displayed/stamped version. |

Installer safety behavior:

- Stages and validates the release bundle before stopping the live runtime.
- Refreshes companion scripts from the resolved release instead of trusting stale installed files.
- Backs up managed install files before mutating `/data/commaview` and restores them if install fails mid-update.
- Clears stale runtime/patch state during install.
- Applies the onroad UI export patch through the patch helper; unsafe patch repair is not automatic.
- Installs the `/data/continue.sh` boot hook `/data/commaview/start.sh --before-openpilot` right before `exec ./launch_openpilot.sh`, replacing the older `/data/commaview/start.sh &` hook. When the install patches the UI while openpilot is running, it says to reboot (see below).

## comma-device lifecycle scripts

| Script | Usage | Notes |
| --- | --- | --- |
| `comma/start.sh` | `bash /data/commaview/start.sh [--before-openpilot]` | Verifies/repairs the UI export patch when safe (offroad only), stops stale processes, then starts `commaviewd bridge` and `commaviewd control`. Never signals openpilot's UI. `--before-openpilot` is the boot hook: it first runs `apply_onroad_ui_export_patch.sh --before-openpilot` synchronously (bounded by `COMMAVIEWD_ONROAD_UI_EXPORT_PREPARE_TIMEOUT_SEC`, default 60), then starts the runtime in the background and returns so `continue.sh` can exec `launch_openpilot.sh`. Uses runtime env listed above. |
| `comma/stop.sh` | `bash /data/commaview/stop.sh` | Stops pidfile-tracked bridge/control processes and cleans stray `/data/commaview/commaviewd` processes. No flags. |
| `comma/uninstall.sh` | `bash /data/commaview/uninstall.sh [--force-offroad]` | Reverts the onroad UI export transformer first and stops without changing anything else if that fails; then stops the runtime, removes the `/data/continue.sh` boot hook, and deletes `/data/commaview`. `--force-offroad` is passed to the revert helper (the app uses it). |

Uninstall from workstation:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/uninstall.sh'
```

Without `--force-offroad`, uninstall refuses while onroad (exit 42). To force the device offroad first:

```bash
ssh comma@<comma-ip> 'bash /data/commaview/uninstall.sh --force-offroad'
```

## Onroad UI export patch scripts

These scripts install the direct v2 socket exporter into upstream openpilot/sunnypilot. `comma/scripts/transform_onroad_ui_export.py` copies `comma/src/commaview_export.<flavor>.py` into the UI and adds the hooks that call it. The change is additive, but it touches live upstream files, so default repair behavior is conservative.

| Script | Usage | Flags/env |
| --- | --- | --- |
| `comma/scripts/verify_onroad_ui_export_patch.sh` | `bash /data/commaview/scripts/verify_onroad_ui_export_patch.sh [--json] [--platform auto\|mici\|tizi\|tici]` | Always prints the status JSON (`--json` is accepted for callers), including `uiReloadPending`/`uiReloadAction`/`uiReloadReason` (below). `COMMAVIEWD_INSTALL_DIR` overrides `/data/commaview`; `COMMAVIEWD_OP_ROOT` overrides `/data/openpilot`; `COMMAVIEWD_PROC_ROOT` overrides `/proc` (tests). |
| `comma/scripts/apply_onroad_ui_export_patch.sh` | `bash /data/commaview/scripts/apply_onroad_ui_export_patch.sh [--force-offroad] [--force-repair] [--before-openpilot] [--platform auto\|mici\|tizi\|tici]` | `--force-offroad` waits for offroad before changing files. `--force-repair` is the only destructive repair path; it backs up targets before reset/reapply. `--before-openpilot` is for the boot hook: when no openpilot manager or UI process is running it skips the onroad check (`IsOffroad` still holds the last session's value until manager clears it) and patches the tree `launch_chffrplus.sh` is about to run, which is `/data/safe_staging/finalized` when a staged openpilot update is about to be installed; when openpilot is running it keeps the onroad check. `COMMAVIEWD_SKIP_OPENPILOT_UI_RESTART=1` skips the UI reload bookkeeping. `COMMAVIEWD_PARAMS_DIR`, `COMMAVIEWD_PROC_ROOT`, `COMMAVIEWD_STAGING_ROOT` override `/data/params/d`, `/proc`, `/data/safe_staging` (tests). |
| `comma/scripts/revert_onroad_ui_export_patch.sh` | `bash /data/commaview/scripts/revert_onroad_ui_export_patch.sh [--force-offroad] [--preflight-only]` | Restores the upstream files (used by `uninstall.sh`). `--preflight-only` checks without changing anything. |

Why the scripts never restart openpilot's UI: openpilot cannot reload its UI in place.

- openpilot (`master`, `release-tizi-staging`, `release-mici-staging`) and sunnypilot `master`: manager never restarts a process that exits. `ensure_running()` calls `PythonProcess.start()`, which returns while `self.proc` is set, and only `stop()` clears it; manager calls `stop()` for `ui` (`always_run`) only when it shuts down. There is no watchdog. The UI exits 0 on SIGINT (`gui_app.init_window` installs a handler that calls `sys.exit(0)`), -15 on SIGTERM; either way it stays dead until reboot. Nothing restarts manager either: `launch_chffrplus.sh` ends in `while true; do sleep 1; done`.
- sunnypilot release branches (`release-tizi`, `release-mici-staging`) restart a dead `ui` (`restart_if_crash=True`, any exit code), but `manager_init()` preimports every process module (`PythonProcess.prepare()`) and the new UI is forked from manager, so it runs the code manager imported at startup, not the patched files.

So patched UI files take effect only when manager starts. The boot hook applies the patch before `launch_openpilot.sh` runs; a patch applied while openpilot runs (install, app repair) is reported by verify as `"uiReloadPending": true`, `"uiReloadAction": "reboot"` with a `uiReloadReason`, and the `reason` text says the export takes effect after the next reboot. These fields reach the app unchanged through `/commaview/status` (`onroadUiExport`) and `/commaview/onroad-ui-export/status`/`repair`. apply records the boot id and uptime of the patch in `run/onroad-ui-export-ui-restart-needed`; verify clears it after a reboot or once a manager that started after the patch is running.

Apply safety rules:

- Never signals openpilot's UI or manager.
- Without `--force-repair`, if verify already passes, apply changes no files and exits 0. `install.sh` always passes `--force-repair`.
- If target files have local changes, apply exits 44 instead of modifying them unless `--force-repair` is given.
- Otherwise it backs up the targets, runs the transformer, and restores the backup if the transformer or the follow-up verify fails.
- `--force-repair` is explicit, offroad-gated through the existing flow, and backs up files under `/data/commaview/backups/onroad-ui-export/<timestamp>` before resetting/reapplying.

## Build scripts

| Script | Usage | Flags/env |
| --- | --- | --- |
| `scripts/install-commaviewd-toolchain.sh` | `bash scripts/install-commaviewd-toolchain.sh` | Installs host + arm64 build dependencies via `sudo apt`. Mutates apt source config and adds arm64 architecture; use only on a build host/runner. No flags. Emits `ARM_CAPNP_SO` and `ARM_KJ_SO`; writes GitHub outputs when `GITHUB_OUTPUT` is set. |
| `commaviewd/scripts/build-ubuntu.sh` | `OP_ROOT=/path/to/openpilot-src commaviewd/scripts/build-ubuntu.sh` | Builds host and aarch64 binaries into `DIST_DIR` (`dist/` by default). Env: `OP_ROOT`, `DIST_DIR`, `HOST_CXX`, `CXX`, `CROSS_CXX`, `COMMAVIEWD_SKIP_ARM=1`, `ARM_CAPNP_SO`, `ARM_KJ_SO`, `PATCHED_MSGQ_LOCAL`. No CLI flags. |
| `tools/release/comma-build-bundle.sh` | `tools/release/comma-build-bundle.sh [--skip-build] [<tag>]` | Builds/stages release bundle under `release/<tag>/`. `--skip-build` uses existing `DIST_DIR` artifacts. `<tag>` overrides `comma/version.env` `RELEASE_TAG`. Env: `DIST_DIR`. |

## Verification scripts

Test on your machine while you work (`scripts/ci-local.sh`); CI is the last check before a change lands. `commaviewd-ci` runs on pushes to `master` and on pull requests once they're ready for review (drafts and other branches run nothing; "Run workflow" checks one by hand). It and the two canaries run `.github/workflows/commaviewd-verify.yml` on their target lists (`ci/targets.json`, `ci/canary-*.json`): `ci/plan-targets.py` resolves them, targets on the same upstream commit share one verification build (the rest check only that their UI export patch applies), and a check that already passed for the same content (commaviewd's inputs and the upstream commit) isn't run again, in CI or a canary. For branch protection, require `verify / result`.

| Script | Usage | Flags/env |
| --- | --- | --- |
| `scripts/ci-local.sh` | `scripts/ci-local.sh [--all \| --list \| <target>...]` | Runs `commaviewd-ci`'s checks locally for the given CI targets (default `sunnypilot-release-pin`, the source releases build from): checks out the upstream source like CI (kept under `CI_LOCAL_SRC_DIR`, default `~/.cache/commaviewd-ci-src`), checks the UI export patch applies, then runs `run-verification.sh` and the telemetry guard. Needs the toolchain above. |
| `commaviewd/scripts/run-verification.sh` | `OP_ROOT=/path/to/openpilot-src commaviewd/scripts/run-verification.sh` | Full verification pipeline: upstream interface guard, reproducible build, binary contract check, unit tests, release smoke bundle. Env: `OP_ROOT`, `DIST_DIR`, `RELEASE_SMOKE_TAG`. |
| `commaviewd/scripts/upstream-interface-guard.sh` | `OP_ROOT=/path/to/openpilot-src commaviewd/scripts/upstream-interface-guard.sh [--telemetry-only] [--manifest <path>]` | Checks that upstream `cereal/services.py` and `log.capnp` have the services and fields the runtime needs (accepting renamed aliases), and that this repo's transformer, apply/verify scripts and exporter template exist. Apply/verify check applicability. Writes manifest to `DIST_DIR` by default. |
| `commaviewd/scripts/reproducible-build.sh` | `OP_ROOT=/path/to/openpilot-src commaviewd/scripts/reproducible-build.sh [--manifest <path>]` | Builds twice with fixed `SOURCE_DATE_EPOCH` and compares host/aarch64 digests. |
| `commaviewd/scripts/binary-contract-check.sh` | `DIST_DIR=/path/to/dist commaviewd/scripts/binary-contract-check.sh [--manifest <path>]` | Validates binary architecture, deps, runpath, size, and bundled runtime libraries. |
| `commaviewd/scripts/run-unit-tests.sh` | `OP_ROOT=/path/to/openpilot-src commaviewd/scripts/run-unit-tests.sh` | Builds the runtime (needs the arm64 toolchain or `COMMAVIEWD_SKIP_ARM=1`), then runs the C++ unit tests, the contract/integration scripts, and pytest over `comma/tests`. Run from the repository root. Env: `OP_ROOT`, compiler env inherited by build script. |
| `scripts/verify-telemetry-hardening.sh` | `bash scripts/verify-telemetry-hardening.sh` | Grep-based guard that raw-only telemetry hardening remains in place and old dev/debug flags/env are absent. No flags. |

Experimental video-source selection: the bridge defaults to the existing full
HEVC encoded services. `COMMAVIEW_VIDEO_SOURCE=livestream` instead subscribes
to the stock H.264 livestream encoded services on the same camera ports; it
does **not** start `encoderd --stream` or change the archival encoder. This mode
requires an H.264-capable client and a separately guarded stock livestream
encoder, and is not compatible with the current HEVC-only Android preview.
Unknown source values fail startup.

## Canary/upstream helper

```bash
scripts/sync-canary-upstream.sh <openpilot|sunnypilot> <ref> [dest-root]
```

Supported refs:

- `openpilot`: `nightly`, `nightly-dev`, `master`, `release-mici-staging`, `release-tizi-staging`, `release-chestnut-staging`
- `sunnypilot`: `dev`, `master`, `staging`, `release-mici-staging`, `release-tizi-staging`, `staging-chestnut`

Default destination is `~/.cache/commaviewd-canary/<upstream>-<ref>/openpilot-src`. The script resolves the current ref SHA, force-checks out that SHA, initializes submodules, and writes `source.env` metadata.

## Test/contract scripts

These are mostly CI-facing but useful for targeted local checks.

| Script | Purpose |
| --- | --- |
| `comma/tests/onroad_ui_export_patch_contract_test.sh` | Static contract check for the socket UI export patch. |
| `comma/tests/openpilot_ui_reload_test.py` | The patch lifecycle never signals openpilot's UI; reload status, `--before-openpilot` boot step (staged updates, timeout) and continue.sh hook upgrade. |
| `comma/tests/onroad_ui_export_canary_applicability_test.sh` | Applies/verifies patch against real openpilot/sunnypilot canary refs. |
| `commaviewd/tests/control_mode_api_contract_test.sh` | Verifies control API routes/contract are present. |
| `commaviewd/tests/local_discovery_contract_test.sh` | Verifies local discovery responder contract. |
| `commaviewd/tests/onroad_ui_export_ci_contract_test.sh` | Ensures workflows align to direct v2 validation. |
| `commaviewd/tests/raw_only_runtime_contract_test.sh` | Guards raw-only runtime behavior. |
| `commaviewd/tests/reproducible_build_test.sh` | Checks the reproducible-build script's `--help`. |
| `commaviewd/tests/runtime_debug_policy_contract_test.sh` | Guards runtime-debug config/policy behavior. |
| `commaviewd/tests/timestamped_video_runtime_contract_test.sh` | Guards timestamped video runtime behavior. |
| `commaviewd/tests/unit_tests_pipeline_test.sh` | Guards unit-test pipeline script presence/help behavior. |

Python contract tests:

```bash
python3 -m pytest comma/tests -q
```

## Standard local verification

```bash
python3 -m pytest comma/tests -q
bash comma/tests/onroad_ui_export_patch_contract_test.sh
bash comma/tests/onroad_ui_export_canary_applicability_test.sh
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

1. Update `comma/version.env` with the target runtime release tag.
2. Run verification:

   ```bash
   commaviewd/scripts/run-verification.sh
   ```

3. Build release bundle:

   ```bash
   tools/release/comma-build-bundle.sh <tag>
   ```

4. Push `master`, then tag with runtime format `v*`.
5. Confirm GitHub Actions release publishes:
   - `commaview-comma-<tag>.tar.gz`
   - `commaview-comma-<tag>.tar.gz.sha256`

## CI targets

Main CI matrix (each target builds and runs the full verification pipeline unless noted):

- `commaai/openpilot@release-mici` (`--platform mici`)
- `commaai/openpilot@release-tizi` (`--platform tizi`)
- `commaai/openpilot@release-chestnut` (`--platform mici` and `--platform tizi`; openpilot for devices with the chestnut external GPU)
- `commaai/openpilot@release-tici` (`--platform tici`, backwards-compatible legacy hook-applicability check only)
- `sunnypilot/sunnypilot@release-mici` (`--platform mici`)
- `sunnypilot/sunnypilot@release-tizi` (`--platform tizi`)
- `sunnypilot/openpilot@<COMMAVIEWD_RELEASE_SUNNYPILOT_REF>` (`--platform mici`): the pinned source the release workflow builds from, read from `ci/upstream-refs.env`

Canaries (Mondays and Thursdays, 07:23/07:53 UTC):

- openpilot: `nightly`, `nightly-dev`, `master`, `release-mici-staging`, `release-tizi-staging`, `release-chestnut-staging` (`nightly`, `nightly-dev` and `master` run the applicability check and telemetry-only guard only)
- sunnypilot: `dev`, `master`, `staging`, `release-mici-staging`, `release-tizi-staging`, `staging-chestnut` (sunnypilot has no chestnut release branch yet)

## Program plans and telemetry references

- `commaviewd/docs/COM-55-onroad-ui-parity-program.md` — historical phased plan for comma-device onroad UI parity (its telemetry-JSON milestone predates the raw-only cutover).
- `commaviewd/docs/ai/telemetry-raw-only-readme.md` — short operator doc.
- `commaviewd/docs/ai/telemetry-raw-only-deep-dive.md` — deep technical doc.

## Related app repository

- **Android app (private):** `RhynoTech/CommaView`

## Safety / legal

- Read `DISCLAIMER.md` before use.
- License: `LICENSE` (All Rights Reserved).
