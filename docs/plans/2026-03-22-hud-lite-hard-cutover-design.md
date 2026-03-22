# HUD-lite Hard Cutover Design

## Context
Road testing established that the toxic variable is not just bandwidth or backlog drain behavior. The bridge now uses a conflated/latest-only sampled `carState` subscriber (`v0.0.16-alpha`) and still triggers `commIssue`/immediate disengage when telemetry is enabled with `carState` alone. Earlier repo archaeology also showed that the original Python `SubMaster`-based bridge path implicitly used `conflate=True`, yet Rhyno remembers it still causing stability issues.

That points to a harder conclusion: **an extra external `carState` subscriber is likely toxic on comma4/mici**, regardless of whether it is Python, C++, or latest-only.

At the same time, exact parity with comma/openpilot HUD overlays still requires the corresponding onroad values.

## Goals
1. Preserve exact parity semantics for the overlay data the Android app needs.
2. Eliminate all direct subscription-based telemetry ingestion from `commaviewd`.
3. Avoid a long-lived openpilot/sunnypilot fork repo if possible.
4. Keep runtime behavior safe: if the export path is unhealthy, telemetry is fully disabled rather than silently falling back to toxic direct subscriptions.

## Non-goals
- No more attempts to make direct raw `carState` subscription safe inside `commaviewd`.
- No dual-path telemetry system with legacy raw fallback.
- No brittle UI-state scraping as the primary design.
- No separate telemetry process in v1.

## Approved Product Direction
### Exact-parity fields are required
The must-have fields are all required for parity:
- speed
- cruise/set speed
- steering angle
- steering pressed
- standstill

### Export more than the bare minimum
Rhyno chose to export the required exact-parity fields **plus** any other overlay telemetry the UI already has, as long as the UI does not need to create new subscribers for them.

## Core Architecture
### Single UI-owned HUD-lite export
Create a small **HUD-lite Cap’n Proto service** that is published from the openpilot/sunnypilot **UI process** using values already present in the UI’s shared `SubMaster`.

This means:
- the UI process keeps its normal shared subscribers
- the exporter repackages already-cached state into one bundled telemetry message
- `commaviewd` subscribes only to that one HUD-lite service
- `commaviewd` no longer subscribes directly to `carState`, `controlsState`, `deviceState`, `modelV2`, etc.

### Why this is the right shape
- avoids an extra external `carState` subscriber
- preserves exact semantics by sourcing values from the same UI-owned shared state the on-device overlays use
- keeps the patch surface small and export-oriented
- gives `commaviewd` one telemetry ingress instead of a service matrix

## Hard Cutover Rule
This is a **strict no-fallback cutover**.

Once HUD-lite is introduced:
- remove all direct subscription-based telemetry code from `commaviewd`
- HUD-lite becomes the only telemetry source
- if HUD-lite is missing or unhealthy, telemetry is fully disabled
- do **not** silently fall back to raw subscriptions for debug or degraded mode

## Data Scope
### Minimum required fields
HUD-lite must include the exact-parity fields:
- speed
- cruise/set speed
- standstill
- steering angle
- steering pressed

### Eligible additional fields
Include only values the UI already has in its shared `SubMaster`, such as relevant overlay-facing data derived from:
- `controlsState`
- `selfdriveState`
- `modelV2`
- `liveCalibration`
- `radarState`
- `deviceState`

If a field would require adding a new UI subscriber, it is out of scope for v1.

## Patch Lifecycle / No-Fork Strategy
A true “no maintenance” answer is impossible if we need new behavior from openpilot/sunnypilot. The approved compromise is:
- no long-lived fork repo
- a **small install-time patchset** carried by `commaviewd`
- patchset is as surgical as possible and limited to HUD-lite export support

### Update behavior
When a user updates openpilot/sunnypilot, the patch may be overwritten.

Approved behavior:
- `commaviewd` install/upgrade records a patch fingerprint/version stamp
- on startup/offroad preflight, `commaviewd` verifies that HUD-lite service definition + publisher are present and healthy
- if patch is missing/stale:
  - telemetry is disabled
  - no raw fallback is used
  - repair is required

### Repair model
- **Offroad:** auto-verify and auto-reapply patch if missing/stale
- **Onroad:** never patch
- **App device list card:** show `Repair HUD Export` action when repair is needed and safe to perform

## Runtime Topology
Rhyno chose **one `commaviewd` binary with separate internal telemetry thread/loop**.

So the runtime topology becomes:
- video bridge loop/thread
- HUD-lite telemetry loop/thread
- control path remains separate

The telemetry thread:
- subscribes only to the HUD-lite service
- owns telemetry queueing/timing/stats
- no longer shares the old raw multi-service subscription path

This provides better isolation than a single shared loop without the operational complexity of a separate telemetry process.

## Failure Model
Healthy telemetry has exactly two states:
- **HUD-lite healthy** → telemetry available
- **HUD-lite unhealthy/missing** → telemetry fully disabled

There is no degraded raw fallback mode.

## Verification Criteria
Before calling the migration complete, prove all of the following:
1. `commaviewd` has no raw telemetry subscribers remaining.
2. HUD-lite service is present and alive on device.
3. Android receives required overlay fields from HUD-lite.
4. Openpilot/sunnypilot remains stable on-road with video + telemetry enabled.
5. Patch loss after upstream update is detected and surfaced as repair-needed, not silently ignored.

## Design Consequences
This design intentionally trades a small maintained patchset for a large reduction in runtime risk and complexity. If exact overlay parity is non-negotiable and direct `carState` subscription is toxic, this is the cleanest remaining path.
