# Raw Telemetry Hard Cutover Implementation Plan

> For Claude: REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

Goal: Make commaviewd telemetry transport raw-only by default so comma does not decode telemetry messages, restoring in-car stability while re-enabling carState delivery.

Architecture: Keep video path unchanged. In bridge runtime, keep latest-only queue drain behavior, but in default runtime path skip telemetry JSON and typed encoding work and forward raw envelopes only. Keep one internal rollback switch for emergency decode fallback.

Tech Stack: C++17 commaviewd runtime, cereal/msgq subscriptions, shell contract tests, existing commaviewd build and unit test scripts, GitHub Actions release workflow.

---

### Task 1: Add AI-facing docs for future agents

Files:
- Create: docs/ai/telemetry-raw-only-readme.md
- Create: docs/ai/telemetry-raw-only-deep-dive.md
- Modify: README.md (add links to docs)
- Test: commaviewd/tests/control_mode_api_contract_test.sh (append doc-link checks)

Step 1: Write failing doc contract check
- Add grep checks for the two new docs and README links, run before creating docs.

Step 2: Run test to verify it fails
Run: commaviewd/tests/control_mode_api_contract_test.sh
Expected: FAIL due to missing docs/links.

Step 3: Write minimal docs + links
- Short doc: operating mode, flags, expected logs, known pitfalls.
- Deep doc: message envelope, service indexes, validation checklist, rollback procedure.

Step 4: Run test to verify it passes
Run: commaviewd/tests/control_mode_api_contract_test.sh
Expected: PASS.

Step 5: Commit
Command: git add docs/ai/telemetry-raw-only-readme.md docs/ai/telemetry-raw-only-deep-dive.md README.md commaviewd/tests/control_mode_api_contract_test.sh
Command: git commit -m docs: add raw-only telemetry operator and deep troubleshooting references

### Task 2: Add failing runtime contract for default raw-only decode bypass

Files:
- Create: commaviewd/tests/raw_only_runtime_contract_test.sh
- Test target: commaviewd/src/bridge_runtime.cc

Step 1: Write failing test
- Assert default runtime path does not call JSON builder or typed encoders unless rollback flag is enabled.
- Assert startup log includes RAW_ONLY_DEFAULT marker.

Step 2: Run test to verify it fails
Run: commaviewd/tests/raw_only_runtime_contract_test.sh
Expected: FAIL because marker and guard do not exist yet.

Step 3: Write minimal implementation hooks
- Add runtime bool example g_telemetry_legacy_decode false.
- Add startup log marker when default path is active.

Step 4: Run test to verify it passes
Run: commaviewd/tests/raw_only_runtime_contract_test.sh
Expected: PASS.

Step 5: Commit
Command: git add commaviewd/tests/raw_only_runtime_contract_test.sh commaviewd/src/bridge_runtime.cc
Command: git commit -m test(runtime): enforce raw-only default telemetry contract

### Task 3: Refactor telemetry hot path to skip decode by default

Files:
- Modify: commaviewd/src/bridge_runtime.cc
- Test: commaviewd/tests/test_telemetry_stats.cpp (extend for raw-only counters if needed)

Step 1: Write failing test update
- Add counter expectations showing raw emit increases without JSON or typed population in default mode.

Step 2: Run test to verify it fails
Run: OP_ROOT=/home/pear/openpilot-src commaviewd/scripts/run-unit-tests.sh
Expected: FAIL in updated telemetry stats test.

Step 3: Write minimal implementation
- Keep latest raw snapshot arrays raw_latest, raw_log_mono, raw_event_which.
- Gate expensive decode branch build_telemetry_json and encode helpers behind rollback bool only.
- In default mode send MSG_META_RAW with raw payload and zero-length typed and json sections.

Step 4: Run tests to verify pass
Run: OP_ROOT=/home/pear/openpilot-src commaviewd/scripts/run-unit-tests.sh
Expected: PASS.

Step 5: Commit
Command: git add commaviewd/src/bridge_runtime.cc commaviewd/tests/test_telemetry_stats.cpp
Command: git commit -m feat(runtime): make telemetry forwarding raw-only by default

### Task 4: Add emergency rollback switch internal only

Files:
- Modify: commaviewd/src/bridge_runtime.cc
- Modify: comma4/start.sh (internal env flag plumb if needed)
- Modify: commaviewd/tests/raw_only_runtime_contract_test.sh

Step 1: Write failing test
- Assert rollback flag exists and re-enables legacy decode path only when explicitly set.

Step 2: Run test to verify it fails
Run: commaviewd/tests/raw_only_runtime_contract_test.sh
Expected: FAIL before flag wiring.

Step 3: Write minimal implementation
- Add hidden CLI or env switch example telemetry legacy decode.
- Keep default false.

Step 4: Run tests to verify pass
Run: commaviewd/tests/raw_only_runtime_contract_test.sh
Expected: PASS.

Step 5: Commit
Command: git add commaviewd/src/bridge_runtime.cc comma4/start.sh commaviewd/tests/raw_only_runtime_contract_test.sh
Command: git commit -m feat(runtime): add internal legacy decode rollback switch

### Task 5: Verification, CI release, and coordination gate

Files:
- Modify: none required unless verification finds issues
- Test: commaviewd/scripts/run-unit-tests.sh, commaviewd/tests/control_mode_api_contract_test.sh, commaviewd/tests/raw_only_runtime_contract_test.sh

Step 1: Run full verification
Command: OP_ROOT=/home/pear/openpilot-src commaviewd/scripts/run-unit-tests.sh
Command: commaviewd/tests/control_mode_api_contract_test.sh
Command: commaviewd/tests/raw_only_runtime_contract_test.sh

Step 2: Confirm clean tree
Command: git status --short --branch

Step 3: Push and tag release
Command: git push origin master
Command: git tag -a vX.Y.Z-alpha -m raw-only telemetry hard cutover
Command: git push origin vX.Y.Z-alpha

Step 4: Verify GitHub release workflow output
Command: gh run list --workflow commaviewd-release --limit 5
Expected: tag run success and release assets uploaded.

Step 5: Coordination gate before Android work
- Stop and check with Rhyno whether parallel UI or UX session already implemented related Android telemetry parsing changes before touching app code.
