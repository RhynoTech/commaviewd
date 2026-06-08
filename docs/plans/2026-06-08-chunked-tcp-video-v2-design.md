# Chunked TCP video transport v2 design

Date: 2026-06-08
Repos: `commaviewd` runtime + CommaView Android app
Status: approved for implementation by Rhyno

## Context

CommaView currently streams each HEVC video frame as one length-prefixed TCP message:

```text
outer_u32_len + MSG_VIDEO + timestamp + width + height + header_len + codec_header + frame_data
```

This is simple, but it has one bad failure mode. If the runtime writes part of a large frame and then hits `EAGAIN`, the length-prefixed stream is corrupted unless the socket is reset. The latest runtime fixes made this much rarer, but the fresh support bundle still showed one compact artifact cluster caused by partial-send backpressure.

We are still pre-stable and V2 has not shipped publicly. Compatibility with older app/runtime pairs is not required for this refactor; local testing will use matching latest APK + latest runtime.

## Goals

- Keep the existing TCP sockets and ports:
  - road video: `8200`
  - wide road video: `8201`
  - driver video: `8202`
  - telemetry: `8203`
- Refactor V2 video transport from one huge frame message into smaller chunk records.
- Let Android discard incomplete abandoned frames without treating that as stream corruption.
- Keep incomplete/partial video frames away from MediaCodec.
- Preserve freshness-first behavior: after loss/backpressure, resume on a fresh keyframe.
- Add diagnostics that prove whether remaining hitches are chunk drops, keyframe waits, socket resets, or app receive/decode delays.

## Non-goals

- No WebRTC, RTP, or UDP transport in this slice.
- No compatibility layer for the old V2 video frame format.
- No manual msgq draining; runtime video msgq is already conflated locally.
- No change to telemetry framing except any small diagnostics needed to validate video behavior.

## Recommended approach: chunked TCP V2

The runtime sends each encoded HEVC frame as a sequence of chunk messages. Each chunk remains length-prefixed at the TCP message layer, so Android can continue reading discrete messages safely. The payload format changes from `MSG_VIDEO` whole-frame payloads to video chunk payloads.

### Wire format

Keep the outer TCP message format:

```text
u32 payload_len_be
payload[payload_len]
```

Use a new chunk payload shape under the existing V2 transport contract. Exact constants can be named during implementation, but the payload should contain:

```text
u8  msg_type                 // video chunk
u32 frame_sequence_be
u16 chunk_index_be
u16 chunk_count_be
u8  flags                    // keyframe, first_chunk, final_chunk
u64 timestamp_ns_be
u32 width_be
u32 height_be
u32 codec_header_len_be      // total frame header bytes
u32 data_len_be              // total frame data bytes
u32 chunk_offset_be          // offset into concatenated header+data frame bytes
u32 chunk_len_be
u8  chunk_bytes[chunk_len]
```

`chunk_offset` addresses a logical frame buffer containing:

```text
codec_header || frame_data
```

Flags are limited to keyframe, first chunk, and final chunk. Header presence is represented by `codec_header_len > 0`, not by a separate flag.

For non-keyframes, `codec_header_len` is normally `0`. For keyframes, header bytes are included at the start of the logical frame buffer.

### Chunk sizing

Start with a conservative chunk size of 16 KiB or 32 KiB. The implementation can use a named constant and tests should not bake in a magic value beyond verifying multiple chunks for large frames.

Smaller chunks reduce the poisoned-byte region if a partial send still happens. Too-small chunks add CPU/syscall overhead. 16-32 KiB is the right first range for pre-stable testing.

## Runtime behavior

1. Read encoded HEVC frames from existing `roadEncodeData`, `wideRoadEncodeData`, and `driverEncodeData` msgq services.
2. Detect keyframes using the existing HEVC IDR detection.
3. Push frames through the existing freshness queue.
4. Chunk each popped frame into length-prefixed video chunk messages.
5. Send chunks with the bounded send path.

Backpressure policy:

- If backpressure occurs before any bytes of a chunk enter the socket, abandon the current frame and wait for a fresh keyframe.
- If backpressure occurs after a partial chunk write, reset the socket as the last-resort safe path. This should be much rarer and bounded because chunks are small.
- If a frame is abandoned between completed chunks, keep the socket alive. Android will see a newer `frame_sequence`, discard the incomplete old frame, and wait for a complete keyframe frame before decoding.

Runtime diagnostics to add:

- chunks sent by stream
- frames chunked by stream
- frame abandon count by stream
- zero-byte chunk backpressure count
- partial-chunk reset count
- max chunk send duration
- max chunks per frame
- keyframe resume/drop counters already present, kept and surfaced

## Android behavior

`HevcStreamReceiver` changes from whole-frame parsing to chunk reassembly.

Receiver rules:

1. Read each length-prefixed payload as today.
2. If payload is metadata/control-compatible, handle it as today.
3. If payload is a video chunk:
   - validate lengths, sequence, chunk index/count, offset, and total expected size;
   - create or update an in-progress reassembly buffer for that camera;
   - copy chunk bytes into the expected offset;
   - mark chunk received.
4. Only when all chunks for a frame are present, emit one complete frame to the existing raw-frame callback / MediaCodec path.
5. If a new frame sequence arrives while the previous frame is incomplete, discard the incomplete old frame and log a diagnostic event.
6. After any discard or stream reset, require the next complete frame to be a keyframe before feeding MediaCodec.

Android diagnostics to add:

- chunks received by camera
- completed frames by camera
- incomplete frame discard count
- duplicate/out-of-order/invalid chunk count
- max reassembly buffer bytes
- max frame assembly time
- keyframe wait after discard/reset
- per-camera receive gap fields where practical

## Camera switching implications

Chunking fixes the partial-frame poison problem. It does not, by itself, eliminate keyframe wait during MICI camera switches.

After chunking is working, keep the previously recommended switch polish as the next likely slice:

- keep old camera displayed until target has a fresh decodable/renderable keyframe;
- self-heal warm supplemental streams;
- fallback if a hot switch waits too long for keyframe.

## Testing strategy

Runtime tests:

- unit test chunk planning for small, exact-boundary, and multi-chunk frames;
- unit test keyframe/header handling in the chunk planner;
- socket/backpressure test proving zero-byte chunk backpressure abandons the frame without corrupting the stream;
- socket/backpressure test proving partial-chunk backpressure still resets/classifies correctly;
- update runtime contract tests to require chunked V2 video framing.

Android tests:

- chunk reassembler completes single- and multi-chunk frames;
- incomplete frame discarded when newer sequence arrives;
- decoder callback is not invoked for incomplete frames;
- post-discard receiver waits for a complete keyframe before emitting again;
- invalid duplicate/out-of-range chunks are rejected and diagnosed.

Integration validation:

- build runtime bundle and APK;
- install both on Rhyno's test comma/app device;
- capture a support bundle while driving/switching cameras;
- compare against `drive-1780914338074`:
  - stream errors should stay near zero;
  - reconnect attempts should stay near zero;
  - partial-chunk resets should be zero or very rare;
  - incomplete frame discards should recover without socket EOF;
  - max network frame gap and total frame handling should remain below the prior ugly-stutter values.

## Risks and mitigations

- **More protocol complexity.** Mitigate with small chunk planner/reassembler units and focused tests.
- **Memory growth on Android reassembly.** Enforce max frame size and single in-progress frame per camera.
- **Out-of-order chunks are unexpected on TCP but malformed input is possible.** Validate and discard safely.
- **Partial chunk writes can still happen.** Chunking makes the failure region small; the safe fallback remains socket reset.
- **Lockstep app/runtime requirement.** Acceptable pre-stable; setup/install validation should keep Rhyno on matching builds.
