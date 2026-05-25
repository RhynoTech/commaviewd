# Runtime Device-Test Artifacts Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a manual, release-grade device-test artifact workflow for both openpilot and sunnypilot without publishing a runtime release.

**Architecture:** A new manual GitHub Actions workflow builds a two-target matrix, runs the same transformer verification and full runtime verification used by CI, then packages release-shaped bundles under `device-test-*` tags and uploads them as short-retention Actions artifacts. Shell contract tests enforce the workflow stays manual-only, validates both upstream families, uploads explicit checksum/manifests, and never touches GitHub Releases or Firebase current-release state.

**Tech Stack:** GitHub Actions YAML, Bash contract tests, existing `commaviewd/scripts/run-verification.sh`, existing `tools/release/comma4-build-bundle.sh`, existing transformer apply/verify scripts.

---

### Task 1: Add a failing workflow contract test

**Files:**
- Create: `commaviewd/tests/device_test_workflow_contract_test.sh`
- Modify: `commaviewd/scripts/run-unit-tests.sh`
- Modify: `commaviewd/tests/unit_tests_pipeline_test.sh`

**Step 1: Create the contract test**

Create `commaviewd/tests/device_test_workflow_contract_test.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
WORKFLOW="$REPO_ROOT/.github/workflows/commaviewd-device-test.yml"

assert_file() {
  [[ -f "$1" ]] || { echo "FAIL: missing $1" >&2; exit 1; }
}

assert_contains() {
  local needle="$1"
  local file="$2"
  local message="$3"
  grep -Fq -- "$needle" "$file" || { echo "FAIL: $message" >&2; exit 1; }
}

assert_not_contains() {
  local needle="$1"
  local file="$2"
  local message="$3"
  if grep -Fq -- "$needle" "$file"; then
    echo "FAIL: $message" >&2
    exit 1
  fi
}

assert_file "$WORKFLOW"

assert_contains "workflow_dispatch:" "$WORKFLOW" "device-test workflow should be manual dispatch only"
assert_not_contains "push:" "$WORKFLOW" "device-test workflow must not run on push"
assert_not_contains "pull_request:" "$WORKFLOW" "device-test workflow must not run on pull request"
assert_contains "name: openpilot-release-mici-staging" "$WORKFLOW" "device-test matrix should include openpilot release-mici-staging"
assert_contains "upstream_repo: commaai/openpilot" "$WORKFLOW" "device-test matrix should include commaai/openpilot"
assert_contains "upstream_ref: release-mici-staging" "$WORKFLOW" "device-test matrix should include openpilot release-mici-staging ref"
assert_contains "name: sunnypilot-staging" "$WORKFLOW" "device-test matrix should include sunnypilot staging"
assert_contains "upstream_repo: sunnypilot/sunnypilot" "$WORKFLOW" "device-test matrix should include sunnypilot/sunnypilot"
assert_contains "upstream_ref: staging" "$WORKFLOW" "device-test matrix should include sunnypilot staging ref"
assert_contains "apply_onroad_ui_export_patch.sh" "$WORKFLOW" "device-test workflow should apply the transformer patch"
assert_contains "verify_onroad_ui_export_patch.sh" "$WORKFLOW" "device-test workflow should verify the transformer patch"
assert_contains "commaviewd/scripts/run-verification.sh" "$WORKFLOW" "device-test workflow should run full verification"
assert_contains "tools/release/comma4-build-bundle.sh" "$WORKFLOW" "device-test workflow should build release-shaped bundles"
assert_contains "actions/upload-artifact" "$WORKFLOW" "device-test workflow should upload artifacts"
assert_contains "retention-days: 7" "$WORKFLOW" "device-test artifacts should be short-retention"
assert_contains "device-test-manifest.json" "$WORKFLOW" "device-test workflow should write a manifest"
assert_contains "sha256sum" "$WORKFLOW" "device-test workflow should print checksum information"
assert_not_contains "gh release" "$WORKFLOW" "device-test workflow must not create or edit GitHub releases"
assert_not_contains "update-firebase-current-release" "$WORKFLOW" "device-test workflow must not update current release pointers"

printf 'PASS: device-test workflow contract validates non-release RC artifacts\n'
```

**Step 2: Make it executable**

Run:

```bash
chmod +x commaviewd/tests/device_test_workflow_contract_test.sh
```

**Step 3: Wire it into the unit-test runner**

In `commaviewd/scripts/run-unit-tests.sh`, add:

```bash
"$ROOT/tests/device_test_workflow_contract_test.sh"
```

near the other shell contract tests.

**Step 4: Wire it into the pipeline script test**

In `commaviewd/tests/unit_tests_pipeline_test.sh`:

- add:

```bash
DEVICE_TEST_WORKFLOW_CONTRACT="$ROOT/tests/device_test_workflow_contract_test.sh"
```

- include `$DEVICE_TEST_WORKFLOW_CONTRACT` in the executable-check loop
- run it with stdout suppressed:

```bash
"$DEVICE_TEST_WORKFLOW_CONTRACT" >/dev/null
```

- assert the runner invokes it:

```bash
grep -Fq 'device_test_workflow_contract_test.sh' "$RUNNER" || { echo "FAIL: run-unit-tests should execute device-test workflow contract"; exit 1; }
```

**Step 5: Run test to verify it fails**

Run:

```bash
commaviewd/tests/device_test_workflow_contract_test.sh
```

Expected: FAIL because `.github/workflows/commaviewd-device-test.yml` does not exist yet.

**Step 6: Commit failing test**

```bash
git add commaviewd/tests/device_test_workflow_contract_test.sh commaviewd/scripts/run-unit-tests.sh commaviewd/tests/unit_tests_pipeline_test.sh
git commit -m "test: cover runtime device-test workflow contract"
```

---

### Task 2: Implement the manual device-test workflow

**Files:**
- Create: `.github/workflows/commaviewd-device-test.yml`

**Step 1: Add workflow skeleton**

Create `.github/workflows/commaviewd-device-test.yml`:

```yaml
name: commaviewd-device-test

on:
  workflow_dispatch:

permissions:
  contents: read

concurrency:
  group: commaviewd-device-test-${{ github.ref }}
  cancel-in-progress: false

env:
  FORCE_JAVASCRIPT_ACTIONS_TO_NODE24: "true"

jobs:
  build-device-test:
    name: device-test (${{ matrix.target.name }})
    runs-on: ubuntu-24.04
    timeout-minutes: 90
    strategy:
      fail-fast: false
      matrix:
        target:
          - name: openpilot-release-mici-staging
            upstream_repo: commaai/openpilot
            upstream_ref: release-mici-staging
          - name: sunnypilot-staging
            upstream_repo: sunnypilot/sunnypilot
            upstream_ref: staging

    steps:
      - name: Checkout CommaView
        uses: actions/checkout@v6
```

**Step 2: Add toolchain and upstream checkout**

Add steps equivalent to `commaviewd-ci.yml`:

- restore apt cache
- run `./scripts/install-commaviewd-toolchain.sh`
- resolve upstream SHA from branch/tag/ref
- restore upstream source cache
- checkout upstream source into `openpilot-src` when cache misses

Use the existing CI workflow as the source of truth for these steps.

**Step 3: Add transformer apply/verify step**

Add:

```yaml
      - name: Onroad UI export transformer apply/verify
        run: |
          set -euxo pipefail
          mkdir -p dist
          COMMAVIEWD_INSTALL_DIR="${{ github.workspace }}/comma4" COMMAVIEWD_OP_ROOT="${{ github.workspace }}/openpilot-src" ./comma4/scripts/apply_onroad_ui_export_patch.sh
          COMMAVIEWD_INSTALL_DIR="${{ github.workspace }}/comma4" COMMAVIEWD_OP_ROOT="${{ github.workspace }}/openpilot-src" ./comma4/scripts/verify_onroad_ui_export_patch.sh --json >/dev/null
          cp "${{ github.workspace }}/comma4/run/onroad-ui-export-status.json" dist/onroad-ui-export-status.json
          git -C "${{ github.workspace }}/openpilot-src" reset --hard -q HEAD
          git -C "${{ github.workspace }}/openpilot-src" clean -fdq
```

**Step 4: Run full verification**

Add:

```yaml
      - name: Run commaviewd verification pipeline
        env:
          OP_ROOT: ${{ github.workspace }}/openpilot-src
          ARM_CAPNP_SO: ${{ steps.armcapnp.outputs.arm_capnp_so }}
          ARM_KJ_SO: ${{ steps.armcapnp.outputs.arm_kj_so }}
          RELEASE_SMOKE_TAG: device-test-smoke-${{ matrix.target.name }}-${{ github.run_id }}
        run: |
          set -euxo pipefail
          commaviewd/scripts/run-verification.sh
```

**Step 5: Build release-shaped device-test bundle**

Add a step that computes short SHA and device-test tag, builds the bundle, writes outputs, and creates `dist/device-test-manifest.json`:

```yaml
      - name: Build device-test bundle
        id: bundle
        env:
          OP_ROOT: ${{ github.workspace }}/openpilot-src
          ARM_CAPNP_SO: ${{ steps.armcapnp.outputs.arm_capnp_so }}
          ARM_KJ_SO: ${{ steps.armcapnp.outputs.arm_kj_so }}
        run: |
          set -euxo pipefail
          short_sha="${GITHUB_SHA::7}"
          tag="device-test-${{ matrix.target.name }}-${short_sha}-${{ github.run_id }}"
          tools/release/comma4-build-bundle.sh "$tag"
          asset="release/${tag}/commaview-comma4-${tag}.tar.gz"
          sha_file="${asset}.sha256"
          sha256="$(awk '{print $1}' "$sha_file")"
          cat > dist/device-test-manifest.json <<JSON
          {
            "kind": "commaviewd-device-test",
            "target": "${{ matrix.target.name }}",
            "commaviewdSha": "${GITHUB_SHA}",
            "commaviewdShortSha": "${short_sha}",
            "githubRunId": "${{ github.run_id }}",
            "githubRunAttempt": "${{ github.run_attempt }}",
            "upstreamRepo": "${{ matrix.target.upstream_repo }}",
            "upstreamRef": "${{ matrix.target.upstream_ref }}",
            "upstreamSha": "${{ steps.upstream.outputs.sha }}",
            "bundleTag": "${tag}",
            "bundlePath": "${asset}",
            "sha256Path": "${sha_file}",
            "sha256": "${sha256}",
            "onroadUiExportStatus": "dist/onroad-ui-export-status.json"
          }
          JSON
          echo "tag=$tag" >> "$GITHUB_OUTPUT"
          echo "asset=$asset" >> "$GITHUB_OUTPUT"
          echo "sha_file=$sha_file" >> "$GITHUB_OUTPUT"
          echo "sha256=$sha256" >> "$GITHUB_OUTPUT"
```

Make sure the heredoc JSON indentation does not include invalid leading shell content.

**Step 6: Upload artifact payload**

Add:

```yaml
      - name: Upload device-test artifact
        uses: actions/upload-artifact@v7
        with:
          name: commaviewd-device-test-${{ matrix.target.name }}-${{ github.sha }}
          path: |
            ${{ steps.bundle.outputs.asset }}
            ${{ steps.bundle.outputs.sha_file }}
            dist/device-test-manifest.json
            dist/reproducible-build-manifest.json
            dist/upstream-interface-manifest.json
            dist/binary-contract.json
            dist/release-smoke-manifest.json
            dist/onroad-ui-export-status.json
          if-no-files-found: error
          retention-days: 7
```

**Step 7: Add summary**

Add an always-running summary step that prints exact identity and safety notes:

```yaml
      - name: Device-test summary
        if: always()
        run: |
          {
            echo "### commaviewd device-test (${{ matrix.target.name }})"
            echo "- This is not a GitHub Release and does not update live alpha/current-release pointers."
            echo "- CommaViewD SHA: \`${GITHUB_SHA}\`"
            echo "- Upstream: \`${{ matrix.target.upstream_repo }}@${{ matrix.target.upstream_ref }}\`"
            echo "- Upstream SHA: \`${{ steps.upstream.outputs.sha }}\`"
            echo "- Bundle tag: \`${{ steps.bundle.outputs.tag }}\`"
            echo "- Bundle: \`${{ steps.bundle.outputs.asset }}\`"
            echo "- SHA256: \`${{ steps.bundle.outputs.sha256 }}\`"
            echo "- Manifest: \`dist/device-test-manifest.json\`"
            echo "- Onroad UI export status: \`dist/onroad-ui-export-status.json\`"
            echo ""
            echo "Install only by exact artifact and checksum. Do not treat this as latest/stable."
          } | tee -a "$GITHUB_STEP_SUMMARY"
```

**Step 8: Run contract test to verify it passes**

Run:

```bash
commaviewd/tests/device_test_workflow_contract_test.sh
```

Expected: `PASS: device-test workflow contract validates non-release RC artifacts`

**Step 9: Commit implementation**

```bash
git add .github/workflows/commaviewd-device-test.yml
git commit -m "ci: add runtime device-test artifact workflow"
```

---

### Task 3: Verify full local pipeline

**Files:**
- No expected source changes.

**Step 1: Shell syntax checks**

Run:

```bash
bash -n commaviewd/tests/device_test_workflow_contract_test.sh
bash -n commaviewd/scripts/run-unit-tests.sh
bash -n commaviewd/tests/unit_tests_pipeline_test.sh
```

Expected: no output, exit 0.

**Step 2: Run focused contract tests**

Run:

```bash
commaviewd/tests/device_test_workflow_contract_test.sh
commaviewd/tests/unit_tests_pipeline_test.sh
```

Expected:

```text
PASS: device-test workflow contract validates non-release RC artifacts
PASS: verification pipeline scripts exist and support --help
```

**Step 3: Run full verification pipeline**

Run:

```bash
commaviewd/scripts/run-verification.sh
```

Expected:

```text
PASS: commaviewd verification pipeline complete
```

**Step 4: Clean generated artifacts**

If verification generated local artifacts, remove or trash generated directories/files that should not be committed:

```bash
trash dist release comma4/config comma4/tests/__pycache__ comma4/run 2>/dev/null || true
```

Then inspect:

```bash
git status --short --branch
```

Expected: only intended tracked source/docs/workflow changes before final commit, or clean after commits.

**Step 5: Commit verification wiring if needed**

If Task 1 wiring was not already committed, commit it now:

```bash
git add commaviewd/scripts/run-unit-tests.sh commaviewd/tests/unit_tests_pipeline_test.sh commaviewd/tests/device_test_workflow_contract_test.sh
git commit -m "test: cover runtime device-test workflow contract"
```

---

### Task 4: Push and verify GitHub workflow availability

**Files:**
- No expected source changes.

**Step 1: Push master**

Run:

```bash
git push origin master
```

Expected: push succeeds.

**Step 2: Confirm workflow is visible**

Run:

```bash
gh workflow list --repo RhynoTech/commaviewd | grep -F 'commaviewd-device-test'
```

Expected: workflow appears.

**Step 3: Optional manual dispatch after Rhyno confirms timing**

Run only when Rhyno wants artifacts built:

```bash
gh workflow run commaviewd-device-test.yml --repo RhynoTech/commaviewd --ref master
```

Expected: workflow starts two matrix jobs.

**Step 4: Watch run if dispatched**

Run:

```bash
gh run list --repo RhynoTech/commaviewd --workflow commaviewd-device-test --limit 1
```

Then:

```bash
gh run watch <run-id> --repo RhynoTech/commaviewd --exit-status
```

Expected: both matrix jobs succeed and upload artifacts.

**Step 5: Report handoff**

Report:

- design doc commit
- test commit
- workflow commit
- local verification output
- pushed SHA
- whether manual workflow was dispatched
- artifact names/checksums if dispatched
