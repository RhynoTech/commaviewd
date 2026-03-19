# Telemetry Raw-Only Mode (Deep Dive)

## Pipeline contract
- Source: msgq subscriptions on comma bridge.
- Queue policy: drain queued samples, keep latest payload for each service.
- Transport: send MSG_META_RAW envelope to Android clients.
- Consumer: Android decodes raw payload and drives overlays/fallback views.

## Envelope expectations
- service index
- event which/type
- logMonoTime
- raw payload bytes
- optional typed/json sections reserved for compatibility (normally empty in raw-only mode).

## Services
- carState, selfdriveState, deviceState, liveCalibration, radarState, modelV2, carControl, carOutput, liveParameters, driverMonitoringState, driverStateV2, onroadEvents, roadCameraState.

## Failure signatures
- If comma decode path regresses: CPU spikes and stability drops under carState load.
- If Android decode regresses: missing overlays despite raw counters increasing.

## Troubleshooting order
1. Confirm bridge mode and startup marker.
2. Confirm raw counter growth per active stream.
3. Confirm Android parser receives expected service indexes.
4. Use rollback switch only for emergency stabilization, then revert.

## Coordination rule
- commaviewd runtime changes land first.
- Before Android parser changes, check with Rhyno to avoid overlap with parallel UI/UX session.
