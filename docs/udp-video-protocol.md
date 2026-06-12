# CommaView UDP video protocol (CVUP)

Shared wire contract between the comma-side runtime (`commaviewd`, sender) and
the Android app (`CommaView`, receiver). Device source of truth:
`commaviewd/src/udp_video_protocol.{h,cpp}`. App source of truth:
`app/src/main/java/com/commaview/app/streaming/UdpVideoPacketProtocol.kt`.

All multi-byte fields are big-endian.

## Ports

| Port | Stream | Transport |
| --- | --- | --- |
| 8200 | road | UDP video + legacy TCP control/telemetry companion |
| 8201 | wide | UDP video + legacy TCP control/telemetry companion |
| 8202 | driver | UDP video |
| 8203 | telemetry | TCP telemetry stream |

Video flows over UDP only. The runtime binds one UDP socket per stream at
startup; no TCP connection is required to start or keep a video stream.

## Common header (all datagrams)

| Offset | Size | Field |
| --- | --- | --- |
| 0 | 4 | magic `0x43565550` ("CVUP") |
| 4 | 1 | version (`1`) |
| 5 | 1 | packet type |
| 6 | 1 | stream id (`1` road, `2` wide, `3` driver) |
| 7 | 1 | reserved (`0`) |
| 8 | 2 | session id |

Packet types: `1` Video, `2` Hello, `3` Heartbeat, `4` Policy,
`5` RepairRequest, `6` RepairStatus (reserved).

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
caps, ~2 s max age) and marks resent packets with the repair-resend flag.
Repair requests are session-scoped and served even while video is suppressed.

## Versioning

Version mismatches are rejected outright; there is no negotiation. Any change
to the layouts above requires bumping the version byte and updating both repos
together (`release.properties` in CommaView pins the compatible runtime tag).
