# carState Conflated Sampling Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Change only `commaviewd` sampled `carState` handling to use a true conflated/latest-only subscriber, then build and deploy a local binary to comma4 for on-road validation without touching openpilot or sunnypilot.

**Architecture:** Keep the fix tightly scoped to `commaviewd/src/bridge_runtime.cc`. `carState` sample mode should use a conflated subscriber and read at most one latest raw message per emit tick, while pass mode and all non-`carState` telemetry services remain unchanged. Use TDD with existing shell contract tests plus a focused runtime contract extension, then verify via local build and on-device testing.

**Tech Stack:** C++ runtime (`commaviewd`), shell contract tests, existing build scripts, SSH deploy to comma4, on-road validation.

---

### Task 1: Add failing contract coverage for carState-specific conflated sampling

**Files:**
- Modify: `commaviewd/tests/runtime_debug_policy_contract_test.sh`
- Modify: `commaviewd/tests/raw_only_runtime_contract_test.sh`
- Test target: `commaviewd/src/bridge_runtime.cc`

**Step 1: Write the failing tests**

Add assertions that encode the new desired behavior:
- `carState` sampled path must use `conflate=true`
- non-`carState` sampled services must keep their current behavior in this first pass
- the generic sampled-service drain loop must no longer apply to `carState`

Suggested assertions:

```bash
grep -Fq 'const bool conflate = (i == car_state_idx) ? true : !service_policy_samples(telem_policies[i]);' "$BRIDGE_CPP" \
  || { echo "FAIL: carState sample path should force conflate"; exit 1; }

grep -Fq 'if (telem_sock_idx == car_state_idx && service_policy_samples(telem_policies[telem_sock_idx])) {' "$BRIDGE_CPP" \
  || { echo "FAIL: missing carState sampled special-case"; exit 1; }
```

**Step 2: Run tests to verify they fail**

Run:
- `cd /home/pear/commaviewd && commaviewd/tests/runtime_debug_policy_contract_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/raw_only_runtime_contract_test.sh`

Expected: `FAIL` because the code still uses non-conflated sampled `carState` with manual drain behavior.

**Step 3: Write minimal test-only refinement if needed**
- Keep the tests small and text-contract based to match existing repo style.
- Avoid adding production code in this task.

**Step 4: Re-run tests to confirm they still fail for the intended reason**

Run the same two commands again.

Expected: same targeted failure, not a syntax/path mistake.

**Step 5: Commit**

```bash
cd /home/pear/commaviewd
git add commaviewd/tests/runtime_debug_policy_contract_test.sh commaviewd/tests/raw_only_runtime_contract_test.sh
git commit -m "test(runtime): add carState conflated sampling contract"
```

### Task 2: Implement the smallest possible carState-only sampled subscriber change

**Files:**
- Modify: `commaviewd/src/bridge_runtime.cc`
- Optional inspect-only reference: `commaviewd/src/telemetry_policy.h`
- Test: `commaviewd/tests/runtime_debug_policy_contract_test.sh`
- Test: `commaviewd/tests/raw_only_runtime_contract_test.sh`

**Step 1: Run failing tests again before touching production code**

Run:
- `cd /home/pear/commaviewd && commaviewd/tests/runtime_debug_policy_contract_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/raw_only_runtime_contract_test.sh`

Expected: both still fail on the new contract checks.

**Step 2: Write minimal production code**

In `commaviewd/src/bridge_runtime.cc`:
- identify the telemetry index for `carState`
- create the `carState` subscriber with `conflate=true` even in sampled mode
- keep existing behavior for all other services
- replace the sampled `carState` drain loop with a latest-only single receive on emit ticks
- do not change pass mode logic
- keep runtime stats coherent (receive/emitted/sample counters should still reflect reality)

Target behavior sketch:

```cpp
const bool is_car_state = (i == car_state_idx);
const bool conflate = is_car_state ? true : !service_policy_samples(telem_policies[i]);
telem_socks[i] = SubSocket::create(ctx, TELEMETRY_SERVICES[i], "127.0.0.1", conflate, true, segment_size);
```

And in the poll/read path:

```cpp
if (telem_sock_idx == car_state_idx && service_policy_samples(telem_policies[telem_sock_idx])) {
  std::unique_ptr<Message> latest(sock->receive(true));
  if (!latest) continue;
  // note receive, replace cached latest, no drain loop
  continue;
}
```

**Step 3: Run tests to verify they pass**

Run:
- `cd /home/pear/commaviewd && commaviewd/tests/runtime_debug_policy_contract_test.sh`
- `cd /home/pear/commaviewd && commaviewd/tests/raw_only_runtime_contract_test.sh`

Expected: `PASS` / zero exit.

**Step 4: Run focused broader verification**

Run:
- `cd /home/pear/commaviewd && commaviewd/tests/unit_tests_pipeline_test.sh`

Expected: passes without regressions in existing runtime contract coverage.

**Step 5: Commit**

```bash
cd /home/pear/commaviewd
git add commaviewd/src/bridge_runtime.cc
git commit -m "feat(runtime): use conflated sampled subscriber for carState"
```

### Task 3: Build local binary and verify artifact

**Files:**
- Modify: none unless build exposes defects
- Build target: local `commaviewd` binary

**Step 1: Run the repo build command after code is green**

Run:
- `cd /home/pear/commaviewd && ./commaviewd/scripts/run-unit-tests.sh`
- `cd /home/pear/commaviewd && ./comma4/build.sh`

If `./comma4/build.sh` is not the active path, inspect the current documented build script and use that exact command instead.

**Step 2: Verify build output exists**

Run:
- `cd /home/pear/commaviewd && find dist release -maxdepth 3 \( -name 'commaviewd' -o -name 'commaview-bridge' -o -name '*.tar.gz' \) | sort`

Expected: a fresh runtime binary or bundle is present for deployment.

**Step 3: Record exact artifact path + commit**

Run:
- `cd /home/pear/commaviewd && git rev-parse --short HEAD`
- `cd /home/pear/commaviewd && git status --short --branch`

Expected: intentional changes only.

**Step 4: Commit build-related adjustments only if required**
- Do not commit generated artifacts unless explicitly requested.
- If code/test fixes were needed during build stabilization, commit them with a narrow message.

**Step 5: Stop if build is not clean**
- Do not deploy a half-broken binary to the comma.

### Task 4: Deploy to comma4 and apply carState-only test profile

**Files:**
- Modify on device: `/data/commaview/commaviewd` (or current deployed binary path)
- Modify on device runtime config only for testing: `/data/commaview/config/runtime-debug.json`

**Step 1: Copy the new binary to the comma**

Run the repo’s existing deploy path if present. Otherwise use the smallest direct copy command.

Example fallback:

```bash
scp /home/pear/commaviewd/dist/.../commaviewd comma@<comma-ip>:/data/commaview/commaviewd
ssh comma@<comma-ip> 'chmod +x /data/commaview/commaviewd'
```

**Step 2: Restart runtime on device**

Run:

```bash
ssh comma@<comma-ip> '/data/commaview/stop.sh >/tmp/commaview-stop.log 2>&1 || true; sleep 1; /data/commaview/start.sh >/tmp/commaview-start.log 2>&1 || true'
```

**Step 3: Apply clean test profile**

Set:
- `carState = sample @ 1 Hz`
- all other optional telemetry services `off`

Verify effective config:

```bash
ssh comma@<comma-ip> 'python3 - <<"PY"
import json
print(json.load(open("/data/commaview/run/runtime-debug-effective.json"))["services"])
PY'
```

**Step 4: Ask Rhyno to run the road test**
- live video on
- engage openpilot/sunnypilot
- report whether `commIssue` / immediate disengage returns

**Step 5: Capture proof immediately after the test**

Run:
- telemetry stats snapshot
- recent `/data/log/swaglog.*` `commIssue` lines
- `commaviewd-bridge.log` tail

Commit nothing in this step.

### Task 5: Decide next move from proof, not vibes

**Files:**
- Modify: none unless follow-up work is explicitly approved
- Evidence target: logs + telemetry stats + on-road result

**Step 1: Evaluate success path**
If the test is stable:
- note that `carState` conflated sampling is a viable base
- prepare a follow-up plan for reintroducing the safe bundle around it

**Step 2: Evaluate failure path**
If `commIssue` still happens:
- conclude that any second `carState` subscriber is likely toxic
- stop further subscriber-tuning experiments
- write a short evidence note pointing toward a different architecture

**Step 3: Save evidence note if useful**
- add a concise report under `docs/reports/` or `memory/2026-03-21.md`
- include commands run, config used, and pass/fail outcome

**Step 4: Final verification before claiming anything**

Run:
- `cd /home/pear/commaviewd && git status --short --branch`
- collect the exact deployed commit hash
- collect the exact runtime config used on device

**Step 5: Report back without pushing**
- summarize what changed
- include proof (tests, build output, deployed commit, logs)
- do not push or tag a release until Rhyno explicitly asks
