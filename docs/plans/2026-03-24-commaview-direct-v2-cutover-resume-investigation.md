# 2026-03-24 Direct V2 Cutover Resume — Runtime + Android Packet Corruption Investigation

## Summary
Captured data shows **the issue is not a pure network reachability problem**; it is consistent with a **runtime/app framing stability regression** in the Direct V2 stream path. While network/adb links were intermittently unstable, the strongest evidence is repeated malformed stream frames and video decoder corruption during active streaming.

- comma4 runtime remained reachable and serving API and stream sockets during active capture windows.
- Android tablet logs repeatedly show `Invalid frame length` and `Unknown msg type`, followed by `HEVC road error: Software caused connection abort` and “road surface never became available”.
- Runtime telemetry showed growing reconnect and send-failure churn (`reconnectCount` in the 70s, `commaViewControl.sendFailureCount` elevated, `sendBackpressureCount` non-zero).
- Stream sockets (`8200`/`8201`/`8202`) showed rapid client churn during capture windows.

This packet-level corruption pattern is exactly what you’d expect from frame framing desynchronization between writer and reader, not a simple one-time disconnect.

---

## Data Collection Snapshot (March 24, 2026)

### Environment / Connectivity
- **comma4 SSH:** `192.168.25.40` (reachable on session start).
- **tablet adb pair/connect:** successfully paired repeatedly at times (e.g. `192.168.25.52:35213`, then connected as `192.168.25.52:35015`), but adb and network would later drop and device moved to `offline`.
- **app state:** CommaView app foregrounded on tablet (`MainActivity` focus visible).

### Comma-side endpoints and status
- `GET /commaview/version` => `{"version":"v0.0.19-alpha", "api_port":5002, "telemetryMode":"direct-v2-ui-export", ...}`
- `/commaview/onroad-ui-export/status` => `patchVerified=true`, `state=patch-verified`, `healthy=false`.
- `/commaview/status`:
  - `onroadUiExport.healthy=false`
  - `runtime telemetry not proven`
  - `reconnectCount` observed climbing (`~70` during late capture)
  - `commaViewControl.sendFailureCount` high (`~60`)
  - `sendBackpressureCount` non-zero (`~21`)

### Stream/socket state observed on comma4
- Active listeners confirmed on ports: `5002` and `8200/8201/8202`.
- Established TCP sessions observed to tablet:
  - `8200 <-> tablet`
  - `8201` and/or `8202 <-> tablet`
- This is consistent with expected road/wide-road/driver stream topology.

### Logs

#### tablet (Android / CommaView)
- `HevcStream`: repeated
  - `Invalid frame length: ...` (huge values)
  - `Unknown msg type: -104`
  - `HEVC road error: Software caused connection abort`
- Recovery loops with repeated reconnect/restart attempts.

#### comma side (commaviewd bridge/control)
- Bridge/control logs and runtime debug values show repeated reconnect/telem send churn and elevated backpressure/failures around the same windows.

---

## Probable root-cause direction (from source + telemetry correlation)

### 1) Direct evidence of possible stream-framing corruption
Hevc parser on Android expects the framing:
1. length prefix (`Int`)
2. payload starting with message type byte (`0x01` video / `0x04` raw telemetry)

Corrupted values like giant `Invalid frame length` indicate the parser is reading a length prefix that is not aligned to sender framing.

### 2) Why this aligns with current runtime implementation
In `commaviewd/src/bridge_runtime.cc`, the same `client_fd` is written by:
- main client handler (video send loop)
- telemetry loop thread (`send_meta_raw_frame`)

Those threads run in parallel with no explicit shared send serialization in `handle_client`/`telemetry_loop`. Given the framing protocol is length-prefixed, unsynchronized concurrent writes on the same socket can interleave and corrupt byte boundaries, creating exactly:
- invalid/huge frame lengths
- unknown msg types
- decode aborts
- reconnect storm

This is currently the strongest implementation-level explanation supported by captured evidence.

Key code observations:
- `handle_client` spawns a telemetry thread for ports with telemetry
- both paths call shared `send_frame(...)` path.
- no dedicated send mutex is present around socket writes in that path.

---

## Does splitting video + telemetry from a single stream help?
**Short answer:** it can help, but it is a workaround unless framed writes are fixed first.

- **Yes, it may help** because telemetry and video currently share the same stream socket and a write interleaving bug there can corrupt both directions of traffic for that socket.
- **But no, not sufficient by itself** because even with split streams, the runtime must still respect framing and protocol ordering per socket.
- **Better immediate fix**: enforce atomic/serialized send on each client socket (single writer path or mutex around all writes) where both video and meta are emitted.
- **Second-phase hardening**: consider splitting video and telemetry into separate sockets if isolation is desired, then update client side as needed.

Recommended order:
1. Serialize all writes per socket (low-risk, high-signal fix).
2. Add regression reproduction with malformed-length assertions + reconnect counter checks.
3. If still unstable, split channels (video-only and meta-only) and gate with compatibility checks.

---

## Evidence-to-Action Checklist

### Immediate canary checks
- If `Invalid frame length` appears again, correlate with `commaviewd` send backtrace and telemetry loop activity in the same second window.
- Confirm whether invalid lengths correlate with `telem_raw` bursts.
- Record exact socket pair for both events from `ss -tnp` and app logs.

### Runtime candidate fix areas
- `commaviewd/src/bridge_runtime.cc`
  - add one send lock per client FD or collapse video/telemetry writes through a single producer-consumer sender queue.
  - verify no parallel `send()` on same fd without lock.

### App-side sanity
- Keep Android parser strictness as-is (`Unknown msg type` + `Invalid frame length` already useful diagnostics).
- Add a small debug tag for first bad length + preceding 8 payload bytes for forensics.

---

## Current status
- We have enough evidence to treat this as a **protocol framing integrity issue** with fallback to runtime stream writer concurrency.
- Network availability itself later degraded (`ping/ssh lost` after capture), but the corruption pattern was observed before total drop.

---

## Related files reviewed
- `commaviewd/src/bridge_runtime.cc`
- `commaviewd/src/router.cpp`
- `commaviewd/src/framing.cpp`
- `CommaView/app/src/main/java/com/commaview/app/streaming/HevcStreamReceiver.kt`
- `CommaView/app/src/main/java/com/commaview/app/connection/DeviceConnection.kt`
- `commaviewd` API endpoints and logs under `/data/commaview/run` and `/data/commaview/logs` during live session
