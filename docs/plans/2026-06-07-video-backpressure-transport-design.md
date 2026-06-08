# Video backpressure transport hardening design

## Context

Recent CommaView support bundles on runtime `v0.0.53-alpha-local-1b892fa` showed user-visible stutter clusters that correlate with runtime/video socket pressure, not Android render, GC, memory, or thermal pressure.

Evidence from `drive-1780872800493`:

- `dropCount=618`, `reconnectCount=83`, stream errors `78`.
- `videoSend.backpressureCount=51`, `partialResetCount=51`, last error `EAGAIN`.
- Runtime events had repeated `peer_disconnect`, `client_connected`, and `client_disconnected` churn across road, wide, and driver video streams.
- App-side EOF clusters align with runtime `video_send backpressure` and partial-send resets.

The current runtime sends each received encoded frame directly to the client socket. If the phone cannot drain TCP fast enough, `send_buffers_bounded()` may hit `EAGAIN`. Zero-byte backpressure is safe to drop because no frame bytes entered the stream. Partial backpressure is fatal because the length-prefixed video frame is already partially written; keeping the socket would corrupt the app-side frame parser.

## Goal

Make video delivery freshness-first and HEVC-safe under pressure:

1. Avoid starting stale frames when a client is already behind.
2. Avoid partial-frame socket corruption whenever possible.
3. Recover from pressure by dropping complete unsent frames, preferably until a clean keyframe boundary.
4. Add diagnostics that explain backpressure storms before they become reconnect storms.

## Non-goals

- Do not change the Android wire protocol for this slice.
- Do not add APK-side camera-switch UI behavior in this runtime slice.
- Do not treat every encoded video frame as durable data; live preview should prefer recent frames over old frames.

## Design

### 1. Runtime outbound video queue

Each video client keeps a small per-stream pending-frame buffer before socket send. The frame object owns the already-extracted frame metadata and encoded bytes/header references needed to send one complete app wire frame.

The queue is intentionally tiny. It should hold enough to absorb short scheduler/network hiccups, but not enough to build latency. A target size of 2-3 frames per stream is the starting point.

When a new frame arrives and the queue is full, the runtime drops queued stale frames before accepting the new one. Dropping happens before any bytes are written to the TCP socket, so it cannot corrupt framing.

### 2. HEVC/keyframe-aware freshness policy

The runtime should classify frames as keyframe/IDR when possible by scanning the HEVC NAL units in `header + data`.

Policy:

- If backlog grows and no bytes from the candidate frame have been sent, discard stale non-keyframes first.
- After pressure/reconnect/drop burst, prefer resuming display from a keyframe/IDR so the Android decoder has a clean boundary.
- If keyframe detection is uncertain, fail safe: do not corrupt the stream; drop before send and let the existing decoder/reconnect behavior recover.

This does not require changing the app protocol. The app already receives `header_len`, `header`, and `data`; runtime only decides which complete frames to send.

### 3. Writer/send loop behavior

The first implementation can stay single-threaded inside `handle_video_client` if tests prove it avoids stale sends. The important behavior is policy, not threads:

- Drain/poll incoming video messages.
- Enqueue fresh frames using the bounded freshness policy.
- Send at most the freshest eligible frame when socket write budget is available.
- On zero-byte backpressure, keep the socket, drop/coalesce unsent stale frames, and wait for a clean keyframe if needed.
- On partial send failure, reset the socket as today because the app wire frame is no longer recoverable.

If the single-threaded loop still starves receive or send under load, split into a producer/consumer writer thread per client in a later slice. Do not add that complexity unless measurements require it.

### 4. Diagnostics

Add runtime counters to `/commaview/debug/runtime` and run events for:

- frames enqueued/dropped/sent per video stream;
- queue depth high-water mark;
- stale-frame drop count;
- keyframe drop/wait/resume count;
- zero-byte video backpressure recovered without reconnect;
- partial-send resets by stream;
- frame age at send and max frame age dropped.

This should make future bundles answer whether stutter came from app decode/render, runtime backlog, socket backpressure, or keyframe starvation.

## Testing strategy

Use TDD before implementation:

1. Unit-test HEVC keyframe classification with synthetic Annex-B NAL payloads for IDR and non-IDR frames.
2. Unit-test queue policy:
   - bounded queue never grows past capacity;
   - newest frames replace stale unsent frames;
   - after pressure, non-keyframes are held/dropped until a keyframe arrives;
   - counters reflect drops and keyframe waits.
3. Existing socket framing tests must continue to pass, especially bounded `EAGAIN` behavior and partial-send classification.
4. Full runtime gate: `OP_ROOT=/home/rhyno/Development/openpilot-src commaviewd/scripts/run-unit-tests.sh`.

## Rollout

Start with runtime-only local build/install on the comma, same narrow CommaView runtime swap used for previous onroad-safe tests. Validate with a switch-heavy drive/support bundle. Success means fewer video partial resets, fewer app EOF stream errors, lower reconnect count, and less visible stutter during pressure clusters.
