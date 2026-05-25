# Runtime Device-Test Artifacts Design

## Goal

Build release-candidate runtime bundles for on-device validation without creating GitHub releases, tags, Firebase/current-release updates, or any "latest" pointer that could affect live alpha testers.

## Context

The onroad UI export runtime integration now uses a source transformer instead of relying on static patch applicability. Before publishing another runtime alpha release, Rhyno needs to validate the transformed runtime patch on real devices across both supported upstream families:

- openpilot `release-mici-staging`
- sunnypilot `staging`

The validation artifacts should be treated with release discipline, but remain private, temporary CI artifacts.

## Chosen approach

Create a dedicated manual GitHub Actions workflow: `.github/workflows/commaviewd-device-test.yml`.

The workflow always runs a two-target matrix:

1. `openpilot-release-mici-staging`
   - upstream repo: `commaai/openpilot`
   - upstream ref: `release-mici-staging`
2. `sunnypilot-staging`
   - upstream repo: `sunnypilot/sunnypilot`
   - upstream ref: `staging`

Each matrix job validates the exact commaviewd commit selected by the workflow run, checks out the matching upstream source, applies and verifies the onroad UI export transformer, runs the full runtime verification pipeline, builds a release-shaped bundle, and uploads that bundle as an Actions artifact.

## Non-goals and guardrails

This workflow must not:

- create or move git tags
- create or edit GitHub releases
- update Firebase or current-release manifests
- publish a stable/latest pointer
- automatically run on every push

Artifacts are commit-SHA addressed and short-lived. Device install instructions must refer to the exact artifact name and checksum.

## Artifact shape

For each target, the workflow uploads one artifact containing:

- `commaview-comma4-device-test-<target>-<short_sha>-<run_id>.tar.gz`
- matching `.sha256`
- `device-test-manifest.json`
- `dist/upstream-interface-manifest.json`
- `dist/binary-contract.json`
- `dist/release-smoke-manifest.json`
- `dist/onroad-ui-export-status.json`

The bundle content remains release-shaped so device validation exercises the same install/runtime layout as an actual release bundle.

## Device-test manifest

The manifest records enough identity to make validation reproducible:

- commaviewd commit SHA and short SHA
- GitHub run ID / attempt
- target name
- upstream repo/ref/SHA
- bundle path
- checksum file path
- onroad UI export status artifact path
- generated timestamp

## Summary output

Each job writes a GitHub step summary with:

- target name
- commaviewd SHA
- upstream repo/ref/SHA
- artifact name
- bundle filename
- SHA256 value
- manifest/status paths
- explicit warning that this is not a release and must be installed by exact artifact/checksum

## Error handling

Failures should stop before upload unless enough diagnostic manifests exist to upload. The workflow uses normal `set -euxo pipefail` behavior for build steps and an always-running summary step. Artifact upload uses `if-no-files-found: error` for the final device-test payload so a green job cannot silently omit the bundle.

## Testing strategy

- Add a shell contract test that validates the workflow is manual-only, builds both upstream targets, avoids release/Firebase publishing, uploads artifacts, and references the release bundle builder.
- Wire the contract test into `commaviewd/scripts/run-unit-tests.sh` and `commaviewd/tests/unit_tests_pipeline_test.sh`.
- Run the new contract test directly.
- Run the full `commaviewd/scripts/run-verification.sh` locally before committing implementation.
- After pushing, manually dispatch the workflow or inspect GitHub Actions syntax if manual dispatch is not appropriate yet.
