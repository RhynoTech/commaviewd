# CommaView telemetry architecture audit

Date: 2026-09-12
Repos reviewed: `RhynoTech/commaviewd`, `RhynoTech/CommaView`, current `commaai/openpilot`
Status: research complete; implementation not started

## Executive conclusion

The current architectural instinct is correct: CommaView telemetry must remain **UI-owned** and must not add direct subscribers to upstream high-frequency services such as `carState`, `controlsState`, or `modelV2`.

Historical road testing already established that even a conflated/latest-only external `carState` subscriber could trigger `commIssue` / immediate disengage on comma 4. That result should be treated as a hard safety constraint, not reopened casually.

The current UI-cache sidecar approach avoids that toxic subscriber matrix, but the implementation puts too much CommaView work directly on the upstream UI hot path. On MICI the upstream UI defaults to 60 FPS, and the injected exporter currently constructs and JSON-serializes many telemetry payloads every `UIState.update()` call, then writes them synchronously through a Unix stream socket. The projection path also sends separately during rendering. This creates avoidable CPU/allocation/syscall load and introduces a bounded-but-real upstream blocking path.

The recommended next architecture is:

1. Keep the existing upstream `UIState.sm` cache as the source of truth.
2. Do not add new subscribers to existing upstream telemetry services.
3. Make the injected UI hook constant-time/bounded and **never wait for CommaView**.
4. Limit live telemetry production to the visual pipeline's useful rate (initially 20 Hz, matching camera/model cadence).
5. Export only changed source domains.
6. Move JSON/binary serialization and Unix socket I/O off the UI thread.
7. Use a bounded latest-value/delta mailbox; if the sidecar is behind, drop stale CommaView telemetry rather than queueing or blocking upstream.
8. Preserve source-domain identity (`carState`, `modelV2`, etc.) even if multiple domains are batched into one IPC/network bundle.
9. Use route `rlog` as the canonical telemetry source for post-drive recording/replay, so the live telemetry path is explicitly lossy/latest-first.
10. Add real upstream-contract canaries that validate service names/fields, not merely patch applicability.

A new external `carState`/`modelV2` subscriber is **not** recommended. A single CommaView-owned msgq aggregate service is worth bench testing as an alternate transport, but only after the non-blocking Unix-socket design is measured; it must never subscribe directly to existing upstream telemetry producers.

---

## 1. Historical safety constraint: direct subscribers are a known bad path

The previous telemetry investigation is decisive. `docs/plans/2026-03-22-hud-lite-hard-cutover-design.md` records that a conflated/latest-only sampled `carState` subscriber still caused `commIssue` / immediate disengage with telemetry enabled. The same document concludes that an extra external `carState` subscriber is likely toxic on comma 4 / MICI regardless of Python vs C++ or conflation.

This is the most important rule for future agents:

> Do not reintroduce direct subscriptions to `carState`, `controlsState`, `modelV2`, `radarState`, etc. as a fallback or convenience path.

If the UI export path is unhealthy, CommaView telemetry should fail disabled/fail open with respect to upstream, not silently fall back to the old subscriber matrix.

---

## 2. Current topology

Current production telemetry is effectively:

```text
openpilot/sunnypilot publishers
        |
        v
existing upstream UI SubMaster
        |
        v
UIState.sm cache
        |
        | injected CommaView exporter
        v
/data/commaview/run/ui-export.sock
        |
        v
commaviewd SocketServer latest-value cache
        |
        v
commaviewd telemetry loop (50 ms poll)
        |
        v
TCP telemetry port 8203
        |
        v
Android TelemetryStreamReceiver
        |
        v
TelemetryClient / JsonRawTelemetryDecoder
        |
        v
source-domain StateFlows -> overlay adapters/renderers
```

`commaviewd`'s current telemetry loop is direct-UI-socket-only. It no longer needs direct source-domain msgq subscribers for parity telemetry. This is good and should remain true.

---

## 3. Critical finding: exporter runs on the upstream UI render/update hot path

`transform_onroad_ui_export.py` injects:

```python
self._commaview_exporter.publish(self)
```

immediately after `device.update()` inside `UIState.update()`.

Current upstream `ui.py` calls `ui_state.update()` from the main GUI render loop. Current openpilot defaults the UI to:

- MICI / comma 4: 60 FPS
- TIZI / comma 3X: 20 FPS
- other/default platforms: 60 FPS unless overridden

Therefore the exporter is not a low-rate side task on comma 4; it is invoked at the 60 Hz UI cadence.

This is especially important because the camera/model video displayed by CommaView is fundamentally a 20 FPS stream. Rebuilding the same model/slow-state telemetry three times per camera frame provides little or no visual value.

### Recommendation

Add an explicit live-export cadence independent of render cadence. Start at **20 Hz maximum** and benchmark lower rates for slow domains.

Do not use UI frame rate as telemetry export rate.

---

## 4. Critical finding: current `publish()` rebuilds every domain even when unchanged

The exporter currently iterates approximately 19 source-domain payload builders on every `publish()` call and then republishes the cached projection.

The payload builders generally check whether a service has ever been received since onroad start (`recv_frame >= started_frame`) rather than whether the source service was updated on the current UI cycle (`sm.updated[service]`).

This means a 20 Hz `modelV2` snapshot can be copied into Python lists and JSON-encoded repeatedly during a 60 Hz MICI UI loop. Slow domains such as `deviceState`, calibration, `onroadEvents`, and effectively-static `carParams` are also rebuilt far more often than their source cadence.

### Recommendation

Track source-domain freshness using the real upstream cache metadata:

- `sm.updated[service]`
- `sm.logMonoTime[service]`
- explicit derived-state generation counters where a payload combines multiple services

Only enqueue a domain when its source generation changed.

For derived domains such as controls/torque values that depend on multiple services, compute a composite generation key from the relevant source `logMonoTime` values.

---

## 5. Critical finding: synchronous JSON + `sendall()` is allowed to block upstream UI

Current `_send_json()` performs all of this inline:

1. Python dict construction in the UI process.
2. `json.dumps(...)`.
3. UTF-8 allocation.
4. frame allocation.
5. packet allocation.
6. `SOCK_STREAM.sendall(...)`.

The socket timeout is 50 ms.

A 50 ms stall is three 60-FPS frame budgets. The code correctly catches failures, but catching a timeout after it happens does not make the UI hot path non-blocking.

There is another failure amplification risk: `_close()` does not install the one-second reconnect backoff that `_connect()` uses after a connect failure. After a send failure, subsequent service publishes in the same `publish()` loop can immediately attempt to reconnect again.

### Recommendation

The upstream UI thread must never own socket I/O.

Preferred pattern:

```text
UI thread
   |
   | bounded snapshot/delta enqueue only
   v
latest-value mailbox (capacity 1 generation / per-domain latest)
   |
   v
CommaView exporter worker
   |- serialization
   |- socket connect/reconnect
   |- non-blocking send
   |- drop accounting
```

The worker must be daemonized, explicitly drop realtime priority if appropriate, and be disposable. If it dies or stalls, upstream UI continues unchanged.

---

## 6. Critical finding: projection is sent twice in the steady-state design

`set_onroad_projection(...)` both:

- updates `_latest_onroad_projection`
- immediately sends the projection over the socket

Then the next `publish()` sends `_latest_onroad_projection` again.

On MICI this can make projection traffic effectively up to roughly twice the UI cadence, in addition to all other source-domain messages.

### Recommendation

`set_onroad_projection()` should become cache-only. It should update a generation/version and return immediately.

The exporter worker should send the latest projection once when its generation changes.

---

## 7. High finding: local IPC packet rate is much higher than necessary

With a successful 60 Hz MICI UI loop, the current code can attempt roughly:

- ~20 per-service writes from `publish()` per UI cycle
- + one immediate projection write during rendering

This is on the order of **1,200+ local Unix-socket writes/second** before accounting for failures or service exceptions.

`commaviewd`, meanwhile, polls its latest-value cache every 50 ms and sends new cached entries onward. The sidecar already implements latest-wins behavior, so most of the high-rate UI-side work is redundant.

### Recommendation

Batch source-domain deltas into **one IPC record per export generation**.

A minimal transition format can retain JSON payloads internally:

```text
TelemetryDeltaBundle
  version
  sequence
  generatedMonoTime
  entryCount
  repeated:
    serviceIndex
    sourceLogMonoTime
    payloadLength
    payloadBytes
```

This preserves the app's source-domain contract while reducing socket calls dramatically.

The bundle can initially contain per-domain JSON bytes so Android models do not need to be redesigned at the same time. A later binary encoding should only be adopted if profiling proves JSON is material.

---

## 8. High finding: current upstream service names have drifted beyond the synthetic smoke fixtures

The current openpilot `release-mici-staging` and `release-tizi-staging` UI `SubMaster` include names such as:

- `extrinsicsCalibration`
- `narrowRoadCameraState`

The checked-in CommaView exporter still directly references legacy names such as:

- `liveCalibration`
- `roadCameraState`

Because `_publish_json()` swallows payload-builder exceptions, this kind of drift can degrade telemetry silently rather than fail the patch verification loudly.

The current synthetic smoke test also constructs a fake `SubMaster` using the older names, so it can pass while the real upstream cache has changed.

### Recommendation

Introduce an explicit upstream source adapter/capability map.

Example:

```python
CALIBRATION_SOURCE = first_present(sm, "extrinsicsCalibration", "liveCalibration")
ROAD_CAMERA_SOURCE = first_present(sm, "narrowRoadCameraState", "roadCameraState")
```

Do not scatter aliases throughout payload builders. Resolve a source-domain contract once at exporter initialization and expose a health report.

The canary must instantiate/inspect the real upstream `UIState.sm.services` list for each supported ref and assert that every required CommaView source domain resolves.

Patch applicability alone is not sufficient.

---

## 9. High finding: current canaries verify transformability, not runtime telemetry semantics

The current canary is useful for detecting changed files/anchors, but it does not prove that:

- required upstream services exist under the names the exporter expects
- required fields exist
- each critical payload builder executes against real upstream-shaped data
- no critical service is being silently skipped by `_publish_json()`

### Recommendation

Add a new `upstream-ui-export-contract-test` that runs against each pinned/canary upstream checkout and produces a machine-readable manifest containing:

```text
upstream ref + SHA
platform
UIState service list
resolved CommaView source aliases
required field probes
critical-domain status
projection hook status
export protocol version
```

Fail CI when a required domain cannot be resolved.

---

## 10. High finding: Android does not preserve road vs wide camera state identity

The current JSON decoder maps both service index 16 (`roadCameraState`) and index 19 (`wideRoadCameraState`) to the same `RawTelemetryDecodeResult.RoadCameraState` type.

`TelemetryClient` then applies both through `applyRoadCameraState()`, which uses the same monotonic key (`"roadCameraState"`) and writes the same `_roadCameraState` flow.

That means wide-road camera state is not represented as a distinct source-domain flow and can compete with/overwrite road-camera state depending on timestamps.

### Recommendation

Add a distinct:

```text
WideRoadCameraState
```

result, monotonic key, state flow, tests, and normalized input field.

This matters more as comma 3 / 3X support and camera-profile switching are added.

---

## 11. Medium finding: runtime sidecar cache is safe but can be simplified after upstream work

`commaviewd::ui_export::SocketServer` is already isolated from upstream UI in its own receiving thread and stores only the latest frame per service.

It currently allocates/copies a packet buffer and takes a shared mutex for receive/cache access. The telemetry loop then polls all 20 service slots every 50 ms.

This is not the primary safety concern because it is outside the upstream UI process. Optimize it after the UI hot path is fixed.

Potential later improvement:

- one latest `TelemetryDeltaBundle` generation instead of 20 independent latest slots
- sequence-numbered snapshots
- single cache swap per generation
- one Android telemetry packet per 50 ms instead of many small packets

---

## 12. Medium finding: Android JSON parsing is intentionally expensive but is on the correct device

Android currently:

1. copies the raw JSON bytes
2. decodes UTF-8
3. parses to a generic `JsonElement` tree
4. maps the tree into typed app models

That can create significant allocation pressure at high packet rates, particularly for `modelV2`, but it occurs on the Android device rather than the comma.

This is therefore lower priority than removing work from the upstream UI thread.

After bundling reduces packet count, benchmark:

- current JSON tree decoder
- generated Kotlin serialization DTOs
- compact binary/Cap'n Proto/protobuf-style normalized payload

Do not move parsing/rendering back to comma merely to optimize Android.

---

## 13. Medium finding: current docs still describe obsolete raw msgq subscription topology

`commaviewd/docs/ai/telemetry-raw-only-deep-dive.md` still says the source is msgq subscriptions on the comma bridge. That is no longer the preferred production architecture.

This is hazardous for agentic development because a future agent can follow the old document and reintroduce the exact subscriber design that road testing rejected.

### Recommendation

Once the new telemetry architecture is implemented, replace the old document with an AI-facing canonical telemetry architecture doc containing explicit hard rules:

- no direct external `carState` subscriber
- no fallback subscriber matrix
- UI-owned cache is source of truth
- exporter must be non-blocking
- route logs are canonical for replay/recording telemetry

---

## 14. Recording/replay implication: live telemetry should not be the archival source

Most of the telemetry domains needed by CommaView are already logged by openpilot into route logs (`rlog`) at their normal logging cadence.

Therefore the recording architecture should eventually become:

```text
LIVE PREVIEW
UI cache -> lossy/latest CommaView telemetry -> Android overlay

POST-DRIVE RECORDING
rlog + camera route files + CommaView recording recipe -> Android renderer
```

Benefits:

- Wi-Fi telemetry loss cannot corrupt the final recording timeline.
- Live telemetry can be dropped under backpressure safely.
- Bench replay and post-drive export consume the same canonical telemetry source.
- No requirement to duplicate every telemetry sample to Android while driving.

Use `rlog`, not decimated `qlog`, for exact replay/recording work where available.

---

## 15. Recommended architecture: UI cache tap v3

### Upstream UI process

Responsibilities:

- inspect existing `UIState.sm` only
- resolve source aliases/capabilities once
- at most 20 Hz, detect changed source generations
- create the minimum immutable snapshot required by changed domains
- enqueue into a bounded latest-value mailbox
- return immediately

Must not:

- open/block on network sockets in the UI thread
- wait for commaviewd
- build all domains when unchanged
- add direct source-domain subscribers
- accumulate unbounded queues

### Export worker

Responsibilities:

- own Unix socket lifecycle
- serialize snapshots
- batch changed domains into one delta bundle
- use non-blocking/bounded local IPC
- maintain drop/reconnect/serialization timing counters

If the worker falls behind:

- replace stale pending state with latest
- never block upstream UI

### IPC transport

Preferred experiment:

- `AF_UNIX`
- `SOCK_SEQPACKET` or another message-preserving local transport
- non-blocking sender
- bounded packet size
- `EAGAIN` => count/drop generation

Why `SOCK_SEQPACKET` is attractive:

- preserves record boundaries
- avoids stream reassembly
- avoids partial application messages
- supports fail-fast backpressure semantics

Keep the existing stream socket as the control/baseline candidate until bench results justify changing it.

### commaviewd

Responsibilities:

- receive latest delta generations
- validate version/sequence
- cache latest source-domain values
- forward one bounded live telemetry bundle to Android at up to 20 Hz
- expose health/stats

### Android

Responsibilities remain source-domain based:

- `uiStateOnroad`
- `selfdriveState`
- `carState`
- `controlsState`
- `modelV2`
- etc.

Wire bundling must not force the app API back into legacy `control/scene/status` buckets. Batching is a transport optimization, not a semantic regression.

---

## 16. Alternative worth bench testing: one CommaView-owned msgq service

The older HUD-lite/direct-v2 plans proposed publishing one or a few **new CommaView-owned services** from the UI process and having `commaviewd` subscribe only to those services.

This is materially different from the known-toxic design because it does not add readers to `carState`, `controlsState`, `modelV2`, etc. The extra reader exists only on a new CommaView publisher.

Potential advantages:

- uses openpilot's native IPC stack
- clean message framing
- potentially simpler lifecycle than a custom Unix socket

Potential disadvantages:

- requires patching cereal schemas/service registry
- increases upstream patch surface and maintenance burden
- still requires proving publisher behavior cannot affect UI timing
- previous project direction intentionally moved away from the custom-schema service approach

Recommendation:

Bench it as **Candidate C**, not as the default implementation. Do not reintroduce it without fault-injection testing showing that a missing/stalled CommaView subscriber cannot impact the UI publisher.

---

## 17. Alternative not recommended initially: tap raw events inside `SubMaster`

A deeper patch could hook `SubMaster.update_msgs()` and duplicate the already-received Cap'n Proto `Event` before `UIState` converts it into domain readers.

Potential benefit:

- avoids Python field-by-field model reconstruction in the UI thread

Risks:

- modifies core upstream messaging code rather than UI-only files
- makes patch maintenance substantially riskier
- raw event copies still cost memory bandwidth
- normalization/decoding must move somewhere else
- easier for upstream changes to break globally

Only investigate this if the bounded 20 Hz async UI exporter cannot meet the measured budget.

---

## 18. Proposed telemetry rate policy

Initial live UI rates should be aligned to actual visual usefulness rather than upstream producer frequency.

Suggested starting policy for testing:

| Domain | Max live export rate |
| --- | ---: |
| `modelV2` / projection / camera state | 20 Hz |
| `carState` / controls / selfdrive / carControl / carOutput | 20 Hz |
| radar / longitudinal plan / driver state | 20 Hz |
| panda/ignition | 10 Hz or on change |
| calibration | 4 Hz or on change |
| device state | 2 Hz or on change |
| onroad events | on change |
| car params | on change / once per drive |

The video path is 20 FPS, so >20 Hz live telemetry should require evidence of a visible/user-facing benefit.

Android can interpolate/animate visual elements locally where smooth 60 Hz motion is desired.

---

## 19. Safety/performance benchmark matrix

Every candidate must be compared against **vanilla upstream** and the **current CommaView patch**.

### Candidates

A. Vanilla upstream (no CommaView telemetry patch)

B. Current synchronous JSON/socket exporter

C. Minimal hardening:
- 20 Hz cap
- changed-domain filter
- projection cache-only
- reconnect backoff

D. Recommended async delta exporter:
- UI snapshot enqueue only
- worker serialization/I/O
- bounded latest-value mailbox
- batched delta IPC

E. Single CommaView-owned msgq aggregate service (research candidate)

### Test platforms

First:
- comma 4 / MICI

Then:
- comma 3X / TIZI
- comma 3 / TICI

### Required upstream combinations

- openpilot supported release/staging ref
- Sunnypilot supported release/staging ref

### Measurements

Upstream UI:
- achieved FPS
- `UIState.update()` duration
- CommaView hook duration p50/p95/p99/max
- render-frame misses
- memory allocation/GC behavior where measurable

System:
- per-core CPU
- memory RSS
- thermal state
- loggerd/camerad/modeld/controlsd timing/health
- process restarts
- `commIssue` / communication faults

Telemetry:
- source generation -> UI tap latency
- tap -> commaviewd latency
- commaviewd -> Android latency
- Android parse latency
- stale/drop counts
- bytes/sec
- IPC records/sec
- Android packets/sec

### Fault injection

While route replay is running, deliberately:

1. kill `commaviewd`
2. freeze `commaviewd`
3. stop reading the Unix socket
4. fill socket buffers
5. repeatedly connect/disconnect the sidecar
6. restart Android telemetry receiver
7. introduce Wi-Fi loss/stalls
8. feed malformed local IPC packets into the sidecar test harness

Invariant:

> No CommaView downstream failure may materially affect upstream UI/process health.

---

## 20. Acceptance criteria for the recommended exporter

These are proposed engineering targets and should be adjusted only from measured evidence.

### Upstream isolation

- zero new direct subscribers to existing parity source services
- no blocking socket calls on UI thread
- no unbounded queue
- downstream unavailable => UI continues normally
- no upstream process crash/restart/commIssue attributable to CommaView

### Hook budget

Target:

- no-change UI fast path: <100 microseconds p99
- snapshot enqueue path: <1 ms p99 on MICI
- no single CommaView hook invocation >2 ms during normal operation

The benchmark must compare against vanilla variance; absolute targets do not override the requirement that upstream behavior remain statistically indistinguishable.

### Live telemetry

- source-domain latency p95 <100 ms under clean local/Wi-Fi conditions
- latest-wins recovery after network stalls
- no need to deliver every historical telemetry state
- no source-domain identity collisions

### Compatibility

- real upstream service-domain resolution passes on every supported ref
- explicit source aliases for known upstream renames
- unsupported required field => CI/runtime health failure, not silent omission

---

## 21. Immediate implementation slices

### Slice 1 — observability only

Add measurements before changing behavior:

- exporter invocation count
- payload build microseconds by domain
- JSON serialization microseconds by domain
- socket send microseconds
- send failures/timeouts
- bytes by domain
- duplicate-generation count
- projection immediate-send count

Do not log per-frame text onroad; counters/rolling summaries only.

### Slice 2 — low-risk current-path hardening

1. Cap exporter work at 20 Hz on MICI.
2. Use source generations to skip unchanged domains.
3. Make `set_onroad_projection()` cache-only.
4. Add send-failure reconnect backoff.
5. Resolve current upstream calibration/road-camera aliases.
6. Add distinct wide-road-camera state on Android.
7. Update canary to validate real upstream service resolution.

This slice should be benchmarked before and after on the lab.

### Slice 3 — async exporter worker

Move serialization/socket I/O out of the UI thread.

UI thread becomes snapshot/delta enqueue only.

### Slice 4 — delta bundle

Batch changed source domains into one local IPC record and, if beneficial, one Android live telemetry record.

### Slice 5 — route-log recording/replay integration

Make post-drive recording/replay read telemetry from `rlog` instead of depending on live telemetry capture.

### Slice 6 — comma 3 / 3X qualification

Run the same telemetry exporter architecture on TICI/TIZI. Device-specific differences belong in the source-adapter/capability layer, not in a separate telemetry architecture.

---

## 22. Device/UI overlay architecture implication

Telemetry should identify the **device platform** separately from the **selected overlay style**.

Recommended device identities:

```text
COMMA_3_TICI
COMMA_3X_TIZI
COMMA_4_MICI
```

The transformer may legitimately use the same legacy UI file-layout target for TICI/TIZI, but that implementation convenience must not collapse device identity in the app/runtime protocol.

Normalized telemetry should feed a device-independent onroad snapshot. Overlay style becomes a separate choice:

```text
telemetry source -> normalized OnroadSnapshot
                              |
                 +------------+-------------+
                 |            |             |
              Comma 4      Comma 3       custom
              overlay      overlay        overlay
```

This lets a comma 4 user select a comma 3-style overlay and vice versa without changing the telemetry source.

---

## 23. Required documentation cleanup

After implementation, update or supersede:

- `commaviewd/docs/ai/telemetry-raw-only-readme.md`
- `commaviewd/docs/ai/telemetry-raw-only-deep-dive.md`
- old plans that imply direct source-domain subscriptions are still an acceptable fallback
- old device-platform wording that treats comma 3 and comma 3X as the same hardware identity

The canonical AI-facing doc must state the historical `carState` instability prominently.

---

## Final recommendation

Do **not** redesign telemetry around new direct upstream subscribers.

Do **not** move rendering/normalization work back onto comma.

Keep the sidecar/cache concept, but make the UI hook behave like an instrumentation tap rather than a transport endpoint:

```text
existing UI cache
      |
      | bounded latest delta; <=20 Hz
      v
async CommaView export worker
      |
      | non-blocking local IPC
      v
commaviewd
      |
      | latest-wins live bundle
      v
Android
```

For final recordings/replays:

```text
route rlog + route camera media + recording recipe -> Android post-render
```

This separation gives CommaView the properties we want simultaneously:

- no toxic source subscriber matrix
- minimal onroad comma work
- downstream failures cannot stall upstream UI
- deterministic replay/test-bench behavior
- live telemetry optimized for freshness rather than archival completeness
- recording telemetry immune to Wi-Fi drops
- one normalized telemetry model capable of driving comma 4, comma 3/3X, and future custom overlays
