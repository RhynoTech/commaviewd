# Video Backpressure Transport Hardening Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce CommaView video stutters/reconnect storms by making runtime video delivery freshness-first, bounded, and HEVC keyframe-aware under socket backpressure.

**Architecture:** Add a small reusable video transport policy module that detects HEVC IDR/keyframes and owns bounded pending-frame/drop decisions. Wire that policy into `bridge_runtime.cc` before socket writes so stale frames are dropped before any TCP bytes are sent. Preserve the existing length-prefixed app protocol and reset sockets only for unrecoverable partial-frame send failures.

**Tech Stack:** C++17, Cap'n Proto/cereal runtime messages, Linux sockets, existing `commaview::net` bounded send helpers, shell/host unit tests.

---

### Task 1: Add HEVC keyframe classification tests

**Files:**
- Create: `commaviewd/tests/test_video_transport_policy.cpp`
- Create: `commaviewd/src/video_transport_policy.h`
- Create: `commaviewd/src/video_transport_policy.cpp`
- Modify as needed: runtime unit-test build script/CMake file that includes other `test_*.cpp` binaries.

**Step 1: Write failing tests**

Create tests for a tiny API:

```cpp
#include "video_transport_policy.h"
#include <cassert>
#include <cstdint>
#include <vector>

using commaview::video::contains_hevc_idr;

static std::vector<uint8_t> annex_b_nal(uint8_t nal_type) {
  return {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(nal_type << 1), 0x01, 0xaa, 0xbb};
}

static void test_detects_idr_w_radl() {
  auto payload = annex_b_nal(19);
  assert(contains_hevc_idr(payload.data(), payload.size()));
}

static void test_detects_idr_n_lp() {
  auto payload = annex_b_nal(20);
  assert(contains_hevc_idr(payload.data(), payload.size()));
}

static void test_non_idr_is_not_keyframe() {
  auto payload = annex_b_nal(1);
  assert(!contains_hevc_idr(payload.data(), payload.size()));
}

int main() {
  test_detects_idr_w_radl();
  test_detects_idr_n_lp();
  test_non_idr_is_not_keyframe();
  return 0;
}
```

**Step 2: Run test to verify red**

Run the narrow test build/runner command used by `commaviewd/scripts/run-unit-tests.sh` after adding it to the test harness.

Expected: compile/link failure or test failure because `video_transport_policy` does not exist.

**Step 3: Implement minimal classifier**

In `video_transport_policy.h` expose:

```cpp
namespace commaview::video {
bool contains_hevc_idr(const uint8_t* data, size_t len);
}
```

Implementation:
- scan Annex-B start codes `00 00 01` and `00 00 00 01`;
- read HEVC nal unit type from `(nal_header_first_byte >> 1) & 0x3f`;
- return true for type `19` or `20`;
- ignore malformed/truncated NALs.

**Step 4: Verify green**

Run the new test binary and then `OP_ROOT=/home/rhyno/Development/openpilot-src commaviewd/scripts/run-unit-tests.sh`.

**Step 5: Commit**

```bash
git add commaviewd/src/video_transport_policy.* commaviewd/tests/test_video_transport_policy.cpp <test build file>
git commit -m "Add HEVC video transport policy tests"
```

---

### Task 2: Add bounded freshness queue policy

**Files:**
- Modify: `commaviewd/src/video_transport_policy.h`
- Modify: `commaviewd/src/video_transport_policy.cpp`
- Modify: `commaviewd/tests/test_video_transport_policy.cpp`

**Step 1: Write failing queue tests**

Add tests for `VideoFrameQueue`:

```cpp
using commaview::video::PendingVideoFrame;
using commaview::video::VideoFrameQueue;

static PendingVideoFrame frame(uint64_t seq, bool keyframe) {
  PendingVideoFrame f;
  f.sequence = seq;
  f.is_keyframe = keyframe;
  f.payload_bytes = 100;
  f.created_at_ms = seq * 10;
  return f;
}

static void test_queue_keeps_latest_when_full() {
  VideoFrameQueue q(2);
  q.push(frame(1, true));
  q.push(frame(2, false));
  q.push(frame(3, false));
  assert(q.size() == 2);
  assert(q.drop_count() == 1);
  assert(q.peek()->sequence == 2);
}

static void test_pressure_waits_for_keyframe_after_drops() {
  VideoFrameQueue q(3);
  q.note_backpressure_without_partial_send();
  q.push(frame(1, false));
  q.push(frame(2, false));
  assert(q.pop_next() == nullptr);
  q.push(frame(3, true));
  auto* next = q.pop_next();
  assert(next != nullptr);
  assert(next->sequence == 3);
  assert(next->is_keyframe);
}
```

**Step 2: Verify red**

Run `test_video_transport_policy`; expected compile failure for missing queue types.

**Step 3: Implement minimal queue**

Add:

```cpp
struct PendingVideoFrame {
  uint64_t sequence = 0;
  bool is_keyframe = false;
  size_t payload_bytes = 0;
  uint64_t created_at_ms = 0;
};

class VideoFrameQueue {
 public:
  explicit VideoFrameQueue(size_t capacity);
  void push(PendingVideoFrame frame);
  void note_backpressure_without_partial_send();
  const PendingVideoFrame* peek() const;
  std::optional<PendingVideoFrame> pop_next();
  size_t size() const;
  uint64_t drop_count() const;
  uint64_t keyframe_wait_drop_count() const;
};
```

Behavior:
- capacity is at least 1;
- on full push, drop oldest frame(s) until there is room;
- after zero-byte backpressure, enter `waiting_for_keyframe`; while waiting, drop non-keyframes before send;
- first keyframe exits `waiting_for_keyframe` and becomes eligible to send.

**Step 4: Verify green**

Run `test_video_transport_policy`, then full runtime unit gate.

**Step 5: Commit**

```bash
git add commaviewd/src/video_transport_policy.* commaviewd/tests/test_video_transport_policy.cpp
git commit -m "Add bounded video freshness policy"
```

---

### Task 3: Wire freshness policy into bridge video loop

**Files:**
- Modify: `commaviewd/src/bridge_runtime.cc`
- Modify: `commaviewd/src/video_transport_policy.h` if frame metadata needs fields.
- Test: add/extend a source contract or host unit test if bridge loop is too hard to harness directly.

**Step 1: Write failing guard test**

Add a source/contract test that fails unless `bridge_runtime.cc`:
- includes `video_transport_policy.h`;
- creates a `VideoFrameQueue` inside `handle_video_client`;
- calls `note_backpressure_without_partial_send()` on zero-byte video backpressure;
- sends frames from `pop_next()` instead of immediately sending every received frame.

This is acceptable as a bridge-loop guard because the surrounding loop is hard to unit-harness without openpilot sockets.

**Step 2: Verify red**

Run the new guard test; expected failure because bridge still sends directly.

**Step 3: Implement wiring**

Inside `handle_video_client`:
- classify each encoded frame with `contains_hevc_idr(header + data)` without large avoidable copies where possible;
- enqueue a `PendingVideoFrame` with sequence/timestamp/width/height/header/data ownership sufficient for later send;
- attempt to send eligible queued frames;
- on `SendStatus::Backpressure && bytes_sent == 0`, record recovered zero-byte backpressure, call `note_backpressure_without_partial_send()`, and continue without disconnecting;
- keep existing disconnect behavior for any partial send or disconnected result.

**Step 4: Verify green**

Run bridge guard test, `test_video_transport_policy`, `test_net_framing`, then full runtime gate.

**Step 5: Commit**

```bash
git add commaviewd/src/bridge_runtime.cc commaviewd/src/video_transport_policy.* commaviewd/tests/<guard-test>
git commit -m "Coalesce stale video before socket send"
```

---

### Task 4: Add runtime diagnostics counters

**Files:**
- Modify: `commaviewd/src/bridge_runtime.cc`
- Tests: update JSON/debug contract tests if present; otherwise add source contract assertions.

**Step 1: Write failing diagnostics test**

Assert `/commaview/debug/runtime` JSON includes:
- `videoSend.queueDropCount`
- `videoSend.keyframeWaitDropCount`
- `videoSend.queueHighWatermark`
- `videoSend.zeroByteBackpressureRecoveredCount`
- `videoSend.maxQueuedFrameAgeMs`

**Step 2: Verify red**

Run the diagnostics contract test; expected failure due missing fields.

**Step 3: Implement counters**

Add fields to `RuntimeVideoSendStats` and update them from queue policy/bridge loop.

**Step 4: Verify green/full gate**

Run targeted diagnostics test, full unit gate.

**Step 5: Commit**

```bash
git add commaviewd/src/bridge_runtime.cc commaviewd/tests/<diagnostics-test>
git commit -m "Expose video backpressure queue diagnostics"
```

---

### Task 5: Build/install local runtime test bundle

**Files:**
- No committed generated artifacts unless explicitly requested.

**Step 1: Final repo checks**

Run:

```bash
git status --short --branch
OP_ROOT=/home/rhyno/Development/openpilot-src commaviewd/scripts/run-unit-tests.sh
```

Expected: tests pass; only ignored/untracked `dist/`/`release/` artifacts remain.

**Step 2: Build comma4 bundle**

Run existing release bundle script used in prior local installs.

Expected: local `v0.0.53-alpha-local-<sha>` bundle and sha256.

**Step 3: Ask before comma install if device state is risky**

Check `IsOnroad` and current runtime. If onroad/active, ask Rhyno before install. If approved, use the narrow CommaView-only runtime swap, not full installer.

**Step 4: Verify install**

Verify bridge/control PIDs, ports `8200/8201/8202/8203/5002`, `/commaview/version`, and restart reason.

**Step 5: Capture evidence request**

Ask Rhyno for a switch-heavy support bundle. Success criteria: fewer video partial resets, fewer app EOF stream errors, lower reconnect count, and stutters reduced during pressure clusters.
