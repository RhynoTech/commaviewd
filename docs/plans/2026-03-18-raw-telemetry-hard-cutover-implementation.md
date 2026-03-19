# Raw Telemetry Hard Cutover Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make commaviewd telemetry transport raw-only by default so comma does not decode telemetry messages, restoring in-car stability while re-enabling carState delivery.

**Architecture:** Keep video path unchanged. In bridge runtime, keep latest-only queue drain behavior, but in default runtime path skip telemetry JSON and typed encoding work and forward raw envelopes only. Keep one internal rollback switch for emergency decode fallback.

**Tech Stack:** C++17 commaviewd runtime, cereal/msgq subscriptions, shell contract tests, existing commaviewd build and unit test scripts, GitHub Actions release workflow.

---

### Task 1: Add AI-facing docs for future agents

**Files:**
- Create: 
- Create: 
- Modify:  (add links to docs)
- Test:  (append doc-link presence check)

**Step 1: Write failing doc contract check**
- Add grep checks for the two new docs and README links, run before creating docs.

**Step 2: Run test to verify it fails**
Run: 
Expected: FAIL due to missing docs/links.

**Step 3: Write minimal docs + links**
- Short doc: operating mode, flags, expected logs, known pitfalls.
- Deep doc: message envelope, service indexes, validation checklist, rollback procedure.

**Step 4: Run test to verify it passes**
Run: 
Expected: PASS.

**Step 5: Commit**


### Task 2: Add failing runtime contract for default raw-only decode bypass

**Files:**
- Create: 
- Test: 

**Step 1: Write failing test**
- Assert default runtime path does not call JSON builder/typed encoders unless rollback flag is enabled.
- Assert default startup log includes explicit  marker.

**Step 2: Run test to verify it fails**
Run: 
Expected: FAIL because marker/guard do not exist yet.

**Step 3: Write minimal implementation hooks**
- Add runtime bool (example: ).
- Add startup log marker when default path is active.

**Step 4: Run test to verify it passes**
Run: 
Expected: PASS.

**Step 5: Commit**


### Task 3: Refactor telemetry hot path to skip decode by default

**Files:**
- Modify: 
- Test:  (extend for raw-only counters if needed)

**Step 1: Write failing test/update**
- Add counter expectations showing raw emit increases without requiring JSON/typed population in default mode.

**Step 2: Run test to verify it fails**
Run: 
Expected: FAIL in updated telemetry stats test.

**Step 3: Write minimal implementation**
- In telemetry event loop, keep latest raw snapshot (, , ).
- Gate expensive decode branch (, ) behind rollback bool only.
- In default mode, send  with raw payload and zero-length typed/json sections.

**Step 4: Run tests to verify pass**
Run: 
Expected: PASS.

**Step 5: Commit**


### Task 4: Add emergency rollback switch (internal only)

**Files:**
- Modify: 
- Modify:  (internal env flag plumb if needed)
- Create:  additions

**Step 1: Write failing test**
- Assert rollback flag exists and re-enables legacy decode path only when explicitly set.

**Step 2: Run test to verify it fails**
Run: 
Expected: FAIL before flag wiring.

**Step 3: Write minimal implementation**
- Add hidden CLI/env switch (example:  or env var).
- Keep default false.

**Step 4: Run tests to verify pass**
Run: 
Expected: PASS.

**Step 5: Commit**


### Task 5: Verification, CI release, and coordination gate

**Files:**
- Modify: none required unless verification finds issues
- Test: , , 

**Step 1: Run full verification**


**Step 2: Commit final verification notes if needed**


**Step 3: Push + tag release**


**Step 4: Verify GitHub release workflow output**
Run: 
Expected: tag run success + release assets uploaded.

**Step 5: Coordination gate before Android work**
- Stop and check with Rhyno whether parallel UI/UX session already implemented related Android telemetry parsing changes before touching app code.

