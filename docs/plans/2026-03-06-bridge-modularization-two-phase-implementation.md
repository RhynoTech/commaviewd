# CommaView Bridge Modularization (Two-Phase) Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Split `bridge/cpp/commaview-bridge.cc` into clear modules without behavior changes (Phase 1), then add reproducible build + tests (Phase 2).

**Architecture:** Phase 1 is a strict extract-and-wire refactor: keep protocol/ports/flags identical, move code into `net`, `video`, `telemetry`, and `control` modules plus a thin `main.cpp`. Phase 2 layers deterministic build and tests on top after parity is proven.

**Tech Stack:** C++17, openpilot msgq/cereal/capnp, existing `bridge/cpp/build-ubuntu.sh`, shell verification scripts.

---

## Module boundaries (frozen for Phase 1)

- `bridge/cpp/src/main.cpp`
  - CLI arg parsing, signal handlers, server startup/wiring only.
- `bridge/cpp/src/net/*`
  - sockets, accept loop, frame read/write, connection lifecycle.
- `bridge/cpp/src/video/*`
  - encode-data routing and per-port video frame path.
- `bridge/cpp/src/telemetry/*`
  - telemetry subscriptions and JSON/meta builders.
- `bridge/cpp/src/control/*`
  - inbound control frame parsing and session policy updates.
- `bridge/cpp/include/commaview/*`
  - shared types/constants/interfaces.

Hard rules for Phase 1:
- Same ports: `8200/8201/8202`
- Same framing (`MSG_VIDEO`, `MSG_META`, `MSG_CONTROL`)
- Same process args/behavior
- No functional changes; refactor only

---

### Task 1: Baseline guardrail + extraction map (COM-44)

**Files:**
- Create: `docs/plans/2026-03-06-bridge-modularization-two-phase-implementation.md` (this file)
- Create: `docs/reports/2026-03-06-bridge-modularization-baseline.md`

**Step 1: Capture baseline behavior contract**
- Record current flags, ports, message types, and launch behavior from `bridge/cpp/commaview-bridge.cc`.

**Step 2: Capture extraction map**
- Map current function groups into net/video/telemetry/control buckets with target file paths.

**Step 3: Commit**
```bash
git add docs/plans/2026-03-06-bridge-modularization-two-phase-implementation.md \
        docs/reports/2026-03-06-bridge-modularization-baseline.md
git commit -m "docs(bridge): freeze two-phase modularization boundaries and baseline"
```

---

### Task 2: Scaffold module tree + headers (COM-45)

**Files:**
- Create: `bridge/cpp/src/main.cpp`
- Create: `bridge/cpp/src/net/{server.cpp,framing.cpp}`
- Create: `bridge/cpp/src/video/{router.cpp}`
- Create: `bridge/cpp/src/telemetry/{builder.cpp,services.cpp}`
- Create: `bridge/cpp/src/control/{policy.cpp,parser.cpp}`
- Create: `bridge/cpp/include/commaview/...`
- Modify: `bridge/cpp/build-ubuntu.sh`

**Step 1:** Add empty/compilable skeleton files with `TODO` stubs.

**Step 2:** Wire build script to compile modular sources (still same output binary names).

**Step 3:** Compile host + aarch64 and verify outputs exist.

**Step 4:** Commit.

---

### Task 3: Extract net module (COM-45)

**Files:**
- Modify: `bridge/cpp/src/net/server.cpp`
- Modify: `bridge/cpp/src/net/framing.cpp`
- Modify: `bridge/cpp/src/main.cpp`

**Step 1:** Move socket/framing/session send-read functions into net module.

**Step 2:** Keep behavior identical (no protocol changes).

**Step 3:** Build + run smoke check.

**Step 4:** Commit.

---

### Task 4: Extract control + telemetry + video modules (COM-46)

**Files:**
- Modify: `bridge/cpp/src/control/*`
- Modify: `bridge/cpp/src/telemetry/*`
- Modify: `bridge/cpp/src/video/router.cpp`
- Modify: `bridge/cpp/src/main.cpp`

**Step 1:** Move control frame parsing + policy updates.

**Step 2:** Move telemetry builders/subscriptions.

**Step 3:** Move encode routing/per-port dispatch.

**Step 4:** Build and run parity smoke checks.

**Step 5:** Commit.

---

### Task 5: Phase 1 parity verification report (COM-47)

**Files:**
- Create: `docs/reports/2026-03-06-bridge-phase1-parity.md`

**Step 1:** Validate ports/framing/flags unchanged.

**Step 2:** Validate no regressions in live stream + telemetry delivery.

**Step 3:** Record evidence and commit report.

---

### Task 6: Phase 2 backlog specs (COM-48, COM-49)

**Files:**
- Create: `docs/plans/2026-03-06-bridge-phase2-repro-build.md`
- Create: `docs/plans/2026-03-06-bridge-phase2-tests-ci.md`

**Step 1:** Define deterministic build inputs/toolchain pinning and artifact verification.

**Step 2:** Define tests for framing/control/telemetry shaping + CI workflow.

**Step 3:** Commit phase-2 planning docs.

