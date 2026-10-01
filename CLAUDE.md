# commaviewd

## Test locally; CI only confirms

GitHub Actions minutes are limited, and `commaviewd-ci` runs a job per target in
`ci/targets.json` (the canaries are 6 jobs each). Test on this machine while you work, and leave
CI for the end of a feature or fix.

- `scripts/ci-local.sh` runs CI's checks for the source releases build from
  (`sunnypilot-release-pin`); name targets (`--list`) or pass `--all` to check every CI target
  before you ask for review. It needs the arm64 cross toolchain:
  `scripts/install-commaviewd-toolchain.sh` (Ubuntu 24.04; it rewrites the apt sources, so only
  on a container or build host).
- To add or change a CI target, edit `ci/targets.json`: the workflow and `scripts/ci-local.sh`
  both read it through `ci/plan-targets.py`.
- Pushing a feature branch runs no CI. Open its pull request as a draft while you iterate, and
  mark it ready for review once `scripts/ci-local.sh --all` passes: that's when CI runs. Every
  push to a ready pull request runs the whole matrix again, so batch fixes into one push.
- Don't use "Run workflow" on the CI, canary or device-test workflows to test work in progress;
  for an upstream branch the canaries cover, check it locally with
  `scripts/sync-canary-upstream.sh` and `OP_ROOT=... commaviewd/scripts/run-verification.sh`.
