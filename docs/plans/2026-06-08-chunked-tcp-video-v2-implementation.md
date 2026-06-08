# Chunked TCP Video V2 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace CommaView V2 video's single large TCP frame payload with chunked frame records so partial/backpressured sends can recover without poisoning whole video sockets.

**Architecture:** Keep existing TCP video ports and telemetry port, but refactor only the V2 video payload shape. Runtime splits each HEVC frame into bounded chunk messages; Android reassembles complete frames and discards incomplete stale sequences before MediaCodec sees them. Runtime and APK are lockstep pre-stable, so no old-format compatibility layer is required.

**Tech Stack:** C++20-ish runtime in `commaviewd`, cereal/msgq HEVC sources, POSIX sockets, Kotlin Android app, `DataInputStream`, `MediaCodec`, Gradle unit tests, shell contract tests.

---

## Preflight

**Files:**
- Read: `/home/rhyno/.openclaw/workspace/WORKFLOW_AUTO_COMMAVIEW_RUNTIME.md`
- Read: `/home/rhyno/.openclaw/workspace/WORKFLOW_AUTO_COMMAVIEW_APP.md`
- Read: `/home/rhyno/Development/commaviewd/docs/plans/2026-06-08-chunked-tcp-video-v2-design.md`

**Step 1: Confirm runtime repo**

Run:

```bash
cd /home/rhyno/Development/commaviewd
pwd
git remote -v
git branch --show-current
git status --short --branch
```

Expected:
- path is `/home/rhyno/Development/commaviewd`
- branch is `master`
- remote is `RhynoTech/commaviewd`

**Step 2: Confirm app repo**

Run:

```bash
cd /home/rhyno/Development/CommaView
pwd
git remote -v
git branch --show-current
git status --short --branch
```

Expected:
- path is `/home/rhyno/Development/CommaView`
- branch is `master`
- remote is `RhynoTech/CommaView`

---

### Task 1: Runtime chunk protocol data model

**Files:**
- Create: `/home/rhyno/Development/commaviewd/commaviewd/src/video_chunk_protocol.h`
- Create: `/home/rhyno/Development/commaviewd/commaviewd/src/video_chunk_protocol.cpp`
- Modify: `/home/rhyno/Development/commaviewd/commaviewd/scripts/run-unit-tests.sh` if new test source registration is explicit there
- Test: `/home/rhyno/Development/commaviewd/commaviewd/tests/test_video_chunk_protocol.cpp`

**Step 1: Write failing unit tests**

Create `commaviewd/tests/test_video_chunk_protocol.cpp` with tests for:

```cpp
void test_single_chunk_frame() {
  VideoFrameForChunking frame;
  frame.sequence = 7;
  frame.timestamp_ns = 1234;
  frame.width = 1344;
  frame.height = 760;
  frame.is_keyframe = true;
  frame.codec_header = {0x01, 0x02};
  frame.data = {0x10, 0x11, 0x12};

  auto chunks = plan_video_chunks(frame, 16);
  assert(chunks.size() == 1);
  assert(chunks[0].chunk_index == 0);
  assert(chunks[0].chunk_count == 1);
  assert(chunks[0].offset == 0);
  assert(chunks[0].bytes.size() == 5);
  assert(chunks[0].is_keyframe);
  assert(chunks[0].is_first);
  assert(chunks[0].is_final);
}

void test_multi_chunk_frame_preserves_offsets() {
  VideoFrameForChunking frame;
  frame.sequence = 9;
  frame.data.resize(40);
  auto chunks = plan_video_chunks(frame, 16);
  assert(chunks.size() == 3);
  assert(chunks[0].offset == 0);
  assert(chunks[1].offset == 16);
  assert(chunks[2].offset == 32);
  assert(chunks[2].bytes.size() == 8);
}

void test_encoded_chunk_round_trips_header_fields() {
  VideoChunk chunk;
  chunk.frame_sequence = 3;
  chunk.chunk_index = 1;
  chunk.chunk_count = 4;
  chunk.flags = VIDEO_CHUNK_KEYFRAME;
  chunk.timestamp_ns = 99;
  chunk.width = 1928;
  chunk.height = 1208;
  chunk.codec_header_len = 6;
  chunk.data_len = 100;
  chunk.offset = 16;
  chunk.bytes = {0xaa, 0xbb};
  auto payload = encode_video_chunk_payload(chunk);
  assert(payload[0] == MSG_VIDEO_CHUNK);
  auto decoded = decode_video_chunk_payload_for_test(payload);
  assert(decoded.frame_sequence == chunk.frame_sequence);
  assert(decoded.offset == chunk.offset);
  assert(decoded.bytes == chunk.bytes);
}
```

Use exact namespaces matching existing runtime style, likely `commaview::video`.

**Step 2: Run test and verify it fails**

Run:

```bash
cd /home/rhyno/Development/commaviewd
OP_ROOT=/home/rhyno/Development/openpilot-src commaviewd/scripts/run-unit-tests.sh
```

Expected: FAIL because `video_chunk_protocol` symbols do not exist.

**Step 3: Implement minimal chunk protocol**

Create `video_chunk_protocol.h/.cpp` with:

```cpp
static constexpr uint8_t MSG_VIDEO_CHUNK = 0x06;
static constexpr uint8_t VIDEO_CHUNK_KEYFRAME = 1 << 0;
static constexpr uint8_t VIDEO_CHUNK_FIRST = 1 << 1;
static constexpr uint8_t VIDEO_CHUNK_FINAL = 1 << 2;
static constexpr size_t DEFAULT_VIDEO_CHUNK_BYTES = 16 * 1024;

struct VideoFrameForChunking {
  uint32_t sequence = 0;
  uint64_t timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool is_keyframe = false;
  std::vector<uint8_t> codec_header;
  std::vector<uint8_t> data;
};

struct VideoChunk {
  uint32_t frame_sequence = 0;
  uint16_t chunk_index = 0;
  uint16_t chunk_count = 0;
  uint8_t flags = 0;
  uint64_t timestamp_ns = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t codec_header_len = 0;
  uint32_t data_len = 0;
  uint32_t offset = 0;
  std::vector<uint8_t> bytes;
};

std::vector<VideoChunk> plan_video_chunks(const VideoFrameForChunking& frame, size_t chunk_bytes);
std::vector<uint8_t> encode_video_chunk_payload(const VideoChunk& chunk);
```

Encoding order must match the design doc:

```text
u8 msg_type
u32 frame_sequence
u16 chunk_index
u16 chunk_count
u8 flags
u64 timestamp_ns
u32 width
u32 height
u32 codec_header_len
u32 data_len
u32 chunk_offset
u32 chunk_len
chunk bytes
```

**Step 4: Run targeted/unit test**

Run:

```bash
cd /home/rhyno/Development/commaviewd
OP_ROOT=/home/rhyno/Development/openpilot-src commaviewd/scripts/run-unit-tests.sh
```

Expected: PASS, including new chunk protocol tests.

**Step 5: Commit runtime chunk model**

Run:

```bash
cd /home/rhyno/Development/commaviewd
git add commaviewd/src/video_chunk_protocol.* commaviewd/tests/test_video_chunk_protocol.cpp commaviewd/scripts/run-unit-tests.sh
git commit -m "Add chunked video protocol model"
```

---

### Task 2: Runtime chunk sender integration

**Files:**
- Modify: `/home/rhyno/Development/commaviewd/commaviewd/src/bridge_runtime.cc`
- Modify: `/home/rhyno/Development/commaviewd/commaviewd/src/video_transport_policy.h`
- Modify: `/home/rhyno/Development/commaviewd/commaviewd/src/video_transport_policy.cpp`
- Test: `/home/rhyno/Development/commaviewd/commaviewd/tests/test_video_transport_policy.cpp`
- Test: `/home/rhyno/Development/commaviewd/commaviewd/tests/test_net_framing.cpp` if socket send behavior needs a new regression

**Step 1: Write failing policy tests**

Add tests proving the policy can mark a frame abandoned after zero-byte chunk backpressure and require a keyframe before resuming:

```cpp
void test_zero_byte_chunk_backpressure_requires_keyframe_resume() {
  VideoFrameQueue q(4);
  q.push(frame(1, false));
  auto first = q.pop_next();
  assert(first.has_value());
  q.note_backpressure_without_partial_send();
  q.push(frame(2, false));
  assert(!q.pop_next().has_value());
  q.push(frame(3, true));
  auto resumed = q.pop_next();
  assert(resumed.has_value());
  assert(resumed->sequence == 3);
}
```

If existing tests already cover this, add chunk-specific naming/counter expectations instead.

**Step 2: Run and verify failure**

Run:

```bash
cd /home/rhyno/Development/commaviewd
OP_ROOT=/home/rhyno/Development/openpilot-src commaviewd/scripts/run-unit-tests.sh
```

Expected: FAIL until chunk sender state/counters exist.

**Step 3: Integrate chunk sending**

In `bridge_runtime.cc`, replace the single scatter/gather whole-frame send inside `handle_video_client` with:

```cpp
commaview::video::VideoFrameForChunking frame;
frame.sequence = static_cast<uint32_t>(queued->sequence);
frame.timestamp_ns = queued->timestamp_ns;
frame.width = queued->width;
frame.height = queued->height;
frame.is_keyframe = queued->is_keyframe;
frame.codec_header = queued->codec_header;
frame.data = queued->data;

const auto chunks = commaview::video::plan_video_chunks(
    frame,
    commaview::video::DEFAULT_VIDEO_CHUNK_BYTES);

for (const auto& chunk : chunks) {
  const auto payload = commaview::video::encode_video_chunk_payload(chunk);
  const auto send_result = send_frame_locked(client_fd, payload.data(), payload.size(), &send_mutex);
  note_video_chunk_send_result(video_service, chunk, send_result);
  if (send_result.status == commaview::net::SendStatus::Backpressure && send_result.bytes_sent == 0) {
    video_queue.note_backpressure_without_partial_send();
    note_video_zero_byte_backpressure_recovered();
    note_video_frame_abandoned(video_service, queued->sequence, chunk.chunk_index);
    break;
  }
  if (send_result.status != commaview::net::SendStatus::Ok) {
    note_runtime_peer_disconnect(video_service, "video_chunk_send", send_result);
    shutdown(client_fd, SHUT_RDWR);
    goto disconnect;
  }
}
```

Only increment `frame_count` when all chunks for that frame sent successfully.

**Step 4: Add runtime diagnostics counters**

Extend `RuntimeState.video_send` with counters:

```cpp
uint64_t chunks_sent = 0;
uint64_t frames_chunked = 0;
uint64_t frame_abandon_count = 0;
uint64_t zero_byte_chunk_backpressure_count = 0;
uint64_t partial_chunk_reset_count = 0;
uint64_t max_chunks_per_frame = 0;
uint64_t max_chunk_send_micros = 0;
```

Include these in `telemetry-stats.json` under `videoSend`.

**Step 5: Run runtime gate**

Run:

```bash
cd /home/rhyno/Development/commaviewd
OP_ROOT=/home/rhyno/Development/openpilot-src commaviewd/scripts/run-unit-tests.sh
```

Expected: `55 passed` or updated count with all tests passing and `PASS: commaviewd unit tests passed`.

**Step 6: Commit runtime integration**

Run:

```bash
cd /home/rhyno/Development/commaviewd
git add commaviewd/src/bridge_runtime.cc commaviewd/src/video_transport_policy.* commaviewd/tests/test_video_transport_policy.cpp commaviewd/tests/test_net_framing.cpp
git commit -m "Send video frames as bounded chunks"
```

---

### Task 3: Runtime contract tests and docs update

**Files:**
- Modify: `/home/rhyno/Development/commaviewd/commaviewd/tests/timestamped_video_runtime_contract_test.sh`
- Modify: `/home/rhyno/Development/commaviewd/commaviewd/tests/video_transport_bridge_contract_test.sh`
- Modify: `/home/rhyno/Development/commaviewd/commaviewd/tests/runtime_split_transport_contract_test.sh` if it asserts the old payload shape
- Modify: `/home/rhyno/Development/commaviewd/docs/plans/2026-06-08-chunked-tcp-video-v2-design.md` only if implementation details changed

**Step 1: Write failing contract assertions**

Update shell tests to assert:

```bash
assert_contains_fixed "MSG_VIDEO_CHUNK" "$BRIDGE_CPP" "runtime must send chunked video payloads"
assert_contains_fixed "plan_video_chunks" "$BRIDGE_CPP" "runtime must plan video chunks before send"
assert_contains_fixed "frame_abandon_count" "$BRIDGE_CPP" "runtime must track abandoned chunked frames"
assert_not_contains_fixed "Legacy contract marker: video used to call send_frame_locked" "$BRIDGE_CPP" "old whole-frame contract marker should be gone"
```

Adjust helper names to match existing shell test helpers.

**Step 2: Run contract tests and verify failures/passes**

Run:

```bash
cd /home/rhyno/Development/commaviewd
commaviewd/tests/timestamped_video_runtime_contract_test.sh
commaviewd/tests/video_transport_bridge_contract_test.sh
commaviewd/tests/runtime_split_transport_contract_test.sh
```

Expected: PASS after Task 2 implementation; if failing, fix contracts or implementation mismatch.

**Step 3: Run runtime full gate**

Run:

```bash
cd /home/rhyno/Development/commaviewd
OP_ROOT=/home/rhyno/Development/openpilot-src commaviewd/scripts/run-unit-tests.sh
```

Expected: PASS.

**Step 4: Commit contracts**

Run:

```bash
cd /home/rhyno/Development/commaviewd
git add commaviewd/tests/*runtime*_test.sh commaviewd/tests/video_transport_bridge_contract_test.sh docs/plans/2026-06-08-chunked-tcp-video-v2-design.md
git commit -m "Update runtime contracts for chunked video"
```

---

### Task 4: Android video chunk reassembler unit

**Files:**
- Create: `/home/rhyno/Development/CommaView/app/src/main/java/com/commaview/app/streaming/VideoChunkProtocol.kt`
- Create: `/home/rhyno/Development/CommaView/app/src/test/java/com/commaview/app/streaming/VideoChunkProtocolTest.kt`

**Step 1: Write failing Kotlin tests**

Create `VideoChunkProtocolTest.kt` with tests:

```kotlin
@Test
fun `single chunk keyframe completes immediately`() {
    val reassembler = VideoChunkReassembler()
    val chunk = VideoChunk(
        frameSequence = 1,
        chunkIndex = 0,
        chunkCount = 1,
        isKeyframe = true,
        isFirst = true,
        isFinal = true,
        timestampNanos = 1234L,
        width = 1344,
        height = 760,
        codecHeaderLength = 2,
        dataLength = 3,
        chunkOffset = 0,
        bytes = byteArrayOf(1, 2, 10, 11, 12),
    )

    val result = reassembler.accept(chunk)
    assertThat(result).isInstanceOf(VideoReassemblyResult.Complete::class.java)
    val frame = (result as VideoReassemblyResult.Complete).frame
    assertThat(frame.isKeyframe).isTrue()
    assertThat(frame.headerLength).isEqualTo(2)
    assertThat(frame.bytes).isEqualTo(byteArrayOf(1, 2, 10, 11, 12))
}

@Test
fun `newer sequence discards incomplete old frame`() {
    val reassembler = VideoChunkReassembler()
    reassembler.accept(chunk(sequence = 1, index = 0, count = 2, offset = 0, bytes = byteArrayOf(1)))
    val result = reassembler.accept(chunk(sequence = 2, index = 0, count = 1, offset = 0, bytes = byteArrayOf(2), keyframe = false))
    assertThat(result).isInstanceOf(VideoReassemblyResult.DiscardedIncomplete::class.java)
}

@Test
fun `post discard requires complete keyframe`() {
    val reassembler = VideoChunkReassembler()
    reassembler.accept(chunk(sequence = 1, index = 0, count = 2, offset = 0, bytes = byteArrayOf(1)))
    reassembler.accept(chunk(sequence = 2, index = 0, count = 1, offset = 0, bytes = byteArrayOf(2), keyframe = false))
    val nonKey = reassembler.accept(chunk(sequence = 3, index = 0, count = 1, offset = 0, bytes = byteArrayOf(3), keyframe = false))
    assertThat(nonKey).isInstanceOf(VideoReassemblyResult.WaitingForKeyframe::class.java)
    val key = reassembler.accept(chunk(sequence = 4, index = 0, count = 1, offset = 0, bytes = byteArrayOf(4), keyframe = true))
    assertThat(key).isInstanceOf(VideoReassemblyResult.Complete::class.java)
}
```

Use the assertion library already present in app tests.

**Step 2: Run and verify failure**

Run:

```bash
cd /home/rhyno/Development/CommaView
./gradlew :app:testDebugUnitTest --tests 'com.commaview.app.streaming.VideoChunkProtocolTest'
```

Expected: FAIL because chunk protocol classes do not exist.

**Step 3: Implement protocol parser/reassembler**

Create `VideoChunkProtocol.kt` with:

```kotlin
internal const val MSG_VIDEO_CHUNK: Byte = 0x06

internal data class VideoChunk(...)
internal data class ReassembledVideoFrame(
    val frameSequence: Long,
    val timestampNanos: Long,
    val width: Int,
    val height: Int,
    val isKeyframe: Boolean,
    val headerLength: Int,
    val bytes: ByteArray,
)

internal sealed interface VideoReassemblyResult {
    data class Complete(val frame: ReassembledVideoFrame) : VideoReassemblyResult
    data class DiscardedIncomplete(val oldSequence: Long, val newSequence: Long) : VideoReassemblyResult
    data class WaitingForKeyframe(val sequence: Long) : VideoReassemblyResult
    data class Invalid(val reason: String) : VideoReassemblyResult
    data object Accepted : VideoReassemblyResult
}
```

Implement:

```kotlin
internal fun decodeVideoChunkPayload(payload: ByteArray): VideoChunk
internal class VideoChunkReassembler(maxFrameBytes: Int = 4 * 1024 * 1024) {
    fun accept(chunk: VideoChunk): VideoReassemblyResult
    fun resetRequireKeyframe()
}
```

Validation rules:
- `payload[0] == MSG_VIDEO_CHUNK`
- `chunkCount > 0`
- `chunkIndex in 0 until chunkCount`
- `codecHeaderLength + dataLength <= maxFrameBytes`
- `chunkOffset + chunkLen <= codecHeaderLength + dataLength`
- single in-progress frame per receiver
- duplicate chunks are invalid or ignored with diagnostics; choose invalid for the first implementation

**Step 4: Run targeted tests**

Run:

```bash
cd /home/rhyno/Development/CommaView
./gradlew :app:testDebugUnitTest --tests 'com.commaview.app.streaming.VideoChunkProtocolTest'
```

Expected: PASS.

**Step 5: Commit Android reassembler**

Run:

```bash
cd /home/rhyno/Development/CommaView
git add app/src/main/java/com/commaview/app/streaming/VideoChunkProtocol.kt app/src/test/java/com/commaview/app/streaming/VideoChunkProtocolTest.kt
git commit -m "Add chunked video reassembler"
```

---

### Task 5: Android receiver integration

**Files:**
- Modify: `/home/rhyno/Development/CommaView/app/src/main/java/com/commaview/app/streaming/HevcStreamReceiver.kt`
- Modify: `/home/rhyno/Development/CommaView/app/src/test/java/com/commaview/app/streaming/HevcStreamReceiverControlTest.kt` or create `/home/rhyno/Development/CommaView/app/src/test/java/com/commaview/app/streaming/HevcStreamReceiverChunkTest.kt`

**Step 1: Write failing receiver test**

Add a test that feeds two video chunk payloads through receiver parsing and asserts the raw-frame callback fires exactly once after final chunk. If `HevcStreamReceiver` is hard to socket-test, extract a package-visible handler function first under TDD:

```kotlin
@Test
fun `receiver emits raw frame only after final chunk`() {
    val emitted = mutableListOf<ByteArray>()
    val receiver = TestableChunkHandler { bytes, offset, length, isKeyframe, timing ->
        emitted += bytes.copyOfRange(offset, offset + length)
    }
    receiver.handlePayload(encodedChunk(sequence = 1, index = 0, count = 2, final = false))
    assertThat(emitted).isEmpty()
    receiver.handlePayload(encodedChunk(sequence = 1, index = 1, count = 2, final = true))
    assertThat(emitted).hasSize(1)
}
```

**Step 2: Run and verify failure**

Run:

```bash
cd /home/rhyno/Development/CommaView
./gradlew :app:testDebugUnitTest --tests 'com.commaview.app.streaming.HevcStreamReceiverChunkTest'
```

Expected: FAIL until receiver is integrated.

**Step 3: Integrate chunk handling**

In `HevcStreamReceiver.kt`:

- add a `VideoChunkReassembler` property per receiver;
- route `MSG_VIDEO_CHUNK` payloads to `decodeVideoChunkPayload()` + `reassembler.accept()`;
- preserve metadata handling as-is;
- remove or stop accepting old `MSG_VIDEO` whole-frame payload if no lockstep fallback is needed;
- convert `ReassembledVideoFrame` to the existing `onRawFrame` path.

Emission rule:

```kotlin
when (val result = reassembler.accept(chunk)) {
    is VideoReassemblyResult.Complete -> handleReassembledFrame(result.frame, headerReadUs, payloadReadUs, collectReceiveTiming)
    is VideoReassemblyResult.DiscardedIncomplete -> DiagnosticsLog.streamEvent(streamCamera, "video_frame_discarded_incomplete", ...)
    is VideoReassemblyResult.WaitingForKeyframe -> DiagnosticsLog.streamEvent(streamCamera, "video_waiting_for_keyframe", ...)
    is VideoReassemblyResult.Invalid -> DiagnosticsLog.streamEvent(streamCamera, "invalid_video_chunk", ...)
    VideoReassemblyResult.Accepted -> Unit
}
```

On socket error/disconnect, call `reassembler.resetRequireKeyframe()`.

**Step 4: Add diagnostics counters/events**

Add stream events:
- `video_chunk_received` rate-limited or summary-only
- `video_frame_reassembled`
- `video_frame_discarded_incomplete`
- `video_waiting_for_keyframe`
- `invalid_video_chunk`

Avoid per-chunk log spam in normal diagnostics. Prefer summary counters unless diagnostics mode is high enough.

**Step 5: Run targeted tests**

Run:

```bash
cd /home/rhyno/Development/CommaView
./gradlew :app:testDebugUnitTest --tests 'com.commaview.app.streaming.VideoChunkProtocolTest' --tests 'com.commaview.app.streaming.HevcStreamReceiverChunkTest'
```

Expected: PASS.

**Step 6: Commit receiver integration**

Run:

```bash
cd /home/rhyno/Development/CommaView
git add app/src/main/java/com/commaview/app/streaming/HevcStreamReceiver.kt app/src/main/java/com/commaview/app/streaming/VideoChunkProtocol.kt app/src/test/java/com/commaview/app/streaming/*Chunk*Test.kt
git commit -m "Receive chunked video frames"
```

---

### Task 6: App diagnostics/report summary for chunked transport

**Files:**
- Modify: `/home/rhyno/Development/CommaView/app/src/main/java/com/commaview/app/diagnostics/DiagnosticEventStats.kt`
- Modify: `/home/rhyno/Development/CommaView/app/src/main/java/com/commaview/app/diagnostics/DiagnosticDriveStore.kt` if persisted summaries need fields
- Test: `/home/rhyno/Development/CommaView/app/src/test/java/com/commaview/app/diagnostics/DiagnosticDriveExportTest.kt` or nearest existing diagnostics report test

**Step 1: Write failing diagnostics test**

Add a test that emits chunk events and verifies the support report includes:

```text
Video incomplete frame discards: 1
Video invalid chunk count: 1
Video keyframe waits: 1
```

**Step 2: Run and verify failure**

Run:

```bash
cd /home/rhyno/Development/CommaView
./gradlew :app:testDebugUnitTest --tests 'com.commaview.app.diagnostics.DiagnosticDriveExportTest'
```

Expected: FAIL until summary fields are parsed/rendered.

**Step 3: Implement summary fields**

Update stats collection to count the new stream event messages:

```kotlin
"video_frame_discarded_incomplete" -> videoIncompleteDiscardCount++
"invalid_video_chunk" -> videoInvalidChunkCount++
"video_waiting_for_keyframe" -> videoKeyframeWaitCount++
```

Add lines to diagnostic report text.

**Step 4: Run diagnostics tests**

Run:

```bash
cd /home/rhyno/Development/CommaView
./gradlew :app:testDebugUnitTest --tests 'com.commaview.app.diagnostics.DiagnosticDriveExportTest'
```

Expected: PASS.

**Step 5: Commit diagnostics**

Run:

```bash
cd /home/rhyno/Development/CommaView
git add app/src/main/java/com/commaview/app/diagnostics app/src/test/java/com/commaview/app/diagnostics
git commit -m "Summarize chunked video diagnostics"
```

---

### Task 7: Cross-repo full verification

**Files:**
- Runtime repo: `/home/rhyno/Development/commaviewd`
- App repo: `/home/rhyno/Development/CommaView`

**Step 1: Runtime full gate**

Run:

```bash
cd /home/rhyno/Development/commaviewd
OP_ROOT=/home/rhyno/Development/openpilot-src commaviewd/scripts/run-unit-tests.sh
git diff --check
```

Expected: all runtime tests pass; no whitespace errors.

**Step 2: App full unit/build gate**

Run:

```bash
cd /home/rhyno/Development/CommaView
./gradlew :app:testDebugUnitTest
./gradlew :app:assembleDebug
git diff --check
```

Expected: all app unit tests pass; debug APK builds; no whitespace errors.

**Step 3: Inspect git status in both repos**

Run:

```bash
cd /home/rhyno/Development/commaviewd
git status --short --branch
cd /home/rhyno/Development/CommaView
git status --short --branch
```

Expected: only known generated/untracked artifacts remain; all source changes committed.

---

### Task 8: Local bundle/APK and live validation prep

Do this only after Task 7 passes.

**Files:**
- Runtime bundle output under `/home/rhyno/Development/commaviewd/release/`
- App APK output under `/home/rhyno/Development/CommaView/app/build/` or GitHub Actions RC artifact if Rhyno wants signed APK

**Step 1: Build local runtime bundle**

Run:

```bash
cd /home/rhyno/Development/commaviewd
TAG="v0.0.53-alpha-local-$(git rev-parse --short HEAD)"
tools/release/comma4-build-bundle.sh "$TAG"
sha256sum "release/$TAG/commaview-comma4-$TAG.tar.gz"
```

Expected: bundle and checksum created.

**Step 2: Build local debug APK for smoke, or ask before signed RC**

Run local smoke build:

```bash
cd /home/rhyno/Development/CommaView
./gradlew :app:assembleDebug
```

Expected: debug APK builds.

If Rhyno wants a signed install candidate, use the existing `android-app-release-candidate` GitHub Actions workflow after pushing app `master`; do not create a tag or GitHub Release unless explicitly requested.

**Step 3: Install runtime only with explicit live-test approval**

Before touching the comma, verify SSH and onroad state:

```bash
ssh -i ~/.ssh/node-keys/id_ed25519_comma4 comma@192.168.138.40 'cat /data/params/d/IsOnroad 2>/dev/null; cat /data/commaview/version.env 2>/dev/null'
```

If `IsOnroad=1`, use the narrow CommaView-only runtime swap pattern already used in prior local tests. Do not force offroad unless Rhyno explicitly approves.

**Step 4: Validation support bundle**

After Rhyno drives/tests, analyze the fresh support bundle and compare against `drive-1780914338074`:

- stream errors
- reconnect/retry attempts
- max network frame gap
- max total frame handling
- video chunk partial resets
- incomplete frame discards
- keyframe waits
- render spikes/thermal/GC indicators

Expected: incomplete frame discards may appear, but socket EOF/reconnect clusters should drop or remain near zero.
