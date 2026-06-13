# CommaView UDP video protocol (CVUP)

Shared wire contract between the comma-side runtime (`commaviewd`, sender) and
the Android app (`CommaView`, receiver). Device source of truth:
`commaviewd/src/udp_video_protocol.{h,cpp}`. App source of truth:
`app/src/main/java/com/commaview/app/streaming/UdpVideoPacketProtocol.kt`.

All multi-byte fields are big-endian.

## Ports

| Port | Stream | Transport |
| --- | --- | --- |
| 8200 | road | UDP video + TCP control companion |
| 8201 | wide | UDP video + TCP control companion |
| 8202 | driver | UDP video + TCP control companion |
| 8203 | telemetry | UDP telemetry snapshots |

Video and live telemetry flow over UDP only. The runtime binds one UDP socket
per stream at startup; no TCP connection is required to start or keep a video or
telemetry stream. Ports 8200/8201/8202 may still accept TCP control companions
for stream policy, but they do not carry live telemetry fallback. UDP 8203 is
the sole live overlay/HUD telemetry data plane for this alpha protocol.

## Common header (all datagrams)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `0x43565550` ("CVUP") |
| 4 | 1 | version (`1`) |
| 5 | 1 | packet type |
| 6 | 1 | stream id (`1` road, `2` wide, `3` driver, `4` telemetry) |
| 7 | 1 | reserved (`0`) |
| 8 | 2 | session id |

Packet types: `1` Video, `2` Hello, `3` Heartbeat, `4` Policy,
`5` RepairRequest, `6` RepairStatus (reserved), `7` TelemetrySnapshot.

## Client lifecycle (app → runtime)

- **Hello** (10 bytes): announces the client endpoint and session id. A new
  session id replaces the previous endpoint for that stream and resets the
  suppress flag.
- **Heartbeat** (11 bytes): byte 10 is the suppress-video flag (`0`/`1`).
  Sent every 250 ms by the app.
- **Policy** (11 bytes): same layout as Heartbeat; sent immediately when the
  suppress preference changes.

Every Hello/Heartbeat/Policy refreshes client liveness. The runtime stops
sending video (and drops the encoder subscription) when no datagram has been
seen for the liveness window (3 s, `UDP_VIDEO_CLIENT_TIMEOUT_NS`). While the
suppress flag is set, frames are dropped on the device without packetizing.

## Video packets (runtime → app)

Fixed 60-byte header followed by payload. Max datagram 1400 bytes; target
payload 1200 bytes per packet.

| Offset | Size | Field |
| --- | --- | --- |
| 0–9 | 10 | common header (packet type `1`) |
| 10 | 2 | flags |
| 12 | 8 | packet sequence (monotonic per stream) |
| 20 | 8 | frame timestamp (nanoseconds) |
| 28 | 4 | frame sequence |
| 32 | 2 | frame packet index |
| 34 | 2 | frame packet count |
| 36 | 4 | frame byte offset |
| 40 | 4 | frame byte length (codec header + frame data) |
| 44 | 4 | codec header length |
| 48 | 4 | width |
| 52 | 4 | height |
| 56 | 4 | payload length |
| 60 | n | payload |

Flags: bit 0 keyframe, bit 1 CSD present, bit 2 frame start, bit 3 frame end,
bit 4 repair resend. Unknown flags are rejected by both sides.

A frame's bytes are the codec header (if any) immediately followed by the
encoded frame data, split into `frame packet count` contiguous slices.

## Repair (app → runtime)

**RepairRequest** uses the 60-byte video header layout with packet type `5`:
frame sequence at offset 28, payload length at offset 56, and the payload is a
list of big-endian `uint16` packet indexes (empty list = resend whole frame).

The runtime serves repairs from a short-lived cache (per-stream/total byte
caps, default 120 frames/stream frame-count cap, ~2 s max age) and marks
resent packets with the repair-resend flag. Repair requests are session-scoped
and served even while video is suppressed.

## Telemetry snapshots (runtime → app, UDP :8203)

Lossy latest-wins overlay/HUD telemetry. The app activates the channel with
the same Hello/Heartbeat lifecycle on stream id `4`; the suppress flag in
Heartbeat/Policy is ignored on this stream. While a client is live the runtime
builds at most one snapshot per telemetry emit tick (50 ms, ≤ 20 Hz, aligned
with the video cadence) from the newest fresh ui-export payloads:

- **Fast tier (every tick):** uiStateOnroad, selfdriveState, carState,
  controlsState, driverMonitoringState, driverStateV2, modelV2, radarState,
  liveCalibration, carControl, longitudinalPlan, onroadProjection.
- **Slow tier (≤ 10 Hz):** carOutput, liveParameters, carParams, deviceState,
  roadCameraState, pandaStatesSummary, wideRoadCameraState.
- **Never in snapshots:** none for the alpha live overlay path; the snapshot set is the live telemetry contract.

A service is included only when its ui-export frame advanced since the last
included snapshot; unchanged state is not resent. Snapshots are fire-and-
forget: there is no repair, no retransmit, and a send failure drops the whole
snapshot (the next one is at most 50 ms away). The app must discard an
incomplete snapshot as soon as a newer sequence arrives and ignore sequences
at or below the last completed one.

**TelemetrySnapshot datagram** (packet type `7`, stream id `4`):

| Offset | Size | Field |
| --- | --- | --- |
| 0–9 | 10 | common header (packet type `7`, stream id `4`) |
| 10 | 8 | snapshot sequence (monotonic) |
| 18 | 8 | snapshot mono time (nanoseconds) |
| 26 | 2 | fragment index |
| 28 | 2 | fragment count |
| 30 | 4 | blob byte offset |
| 34 | 4 | blob byte length (total) |
| 38 | 2 | payload length |
| 40 | n | payload (contiguous blob slice) |

**Snapshot blob** (after reassembly):

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 1 | blob version (`1`) |
| 1 | 1 | entry count |
| 2 | n | entries |

Each entry: `service index` (1), `flags` (1, reserved `0`), `age ms` (4, wall
age of the payload at build time), `payload length` (4), then the unmodified
ui-export JSON payload for that service.

**TCP interaction:** none for live telemetry in this alpha protocol. UDP `8203`
TelemetrySnapshot datagrams are the only live overlay/HUD telemetry data plane.
TCP/API `5002` remains available for control, pairing, and diagnostics, but the
runtime does not negotiate, demote, or fall back to TCP telemetry for live
overlay state. Compatibility is handled by the release matrix: use a matching
Android APK/runtime pair.

## Versioning

Version mismatches are rejected outright; there is no negotiation. Any change
to the layouts above requires bumping the version byte and updating both repos
together (`release.properties` in CommaView pins the compatible runtime tag).
The alpha UDP telemetry snapshot path is matrix-pinned with the matching Android app/runtime release line; older app/runtime combinations are unsupported rather than negotiated in-protocol.
