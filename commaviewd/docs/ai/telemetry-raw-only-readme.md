# Telemetry Raw-Only Mode (Operator Quick Reference)

## What is live now
- The CommaView exporter runs inside the openpilot/sunnypilot UI process. It builds one compact JSON payload per service and sends it to the bridge over `/data/commaview/run/ui-export.sock`.
- The bridge keeps the latest payload per service and forwards it unchanged to the app as a raw (`0x04`, envelope v5) frame. It does no capnp decode and builds no telemetry JSON of its own.
- This is the only telemetry path. There is no runtime switch back to the old bridge-side JSON decode.

## Why
- Keep decode work off the bridge, especially for high-rate services like carState.
- Improve openpilot engagement stability while keeping full telemetry available to the app.

## Expected runtime signals
- The bridge startup log line includes `[RAW_ONLY_DEFAULT]`, `[DIRECT_V2_UI_EXPORT_DEFAULT]`, `[META_MODE=raw-only]` and `[UI_SOCKET_PREFERRED=on]`.
- In `/data/commaview/run/telemetry-stats.json`, each service's `emittedCount` grows while a client is connected, and `uiExportSocket.connected` is true.

## Rolling back
- To go back to an older behavior, install an older release tag. Do not expose any rollback in normal UI flows.

## Validation checklist
1. Confirm the startup markers above in the bridge log.
2. Confirm carState is emitted (`services.carState.emittedCount` grows).
3. Confirm stable drive engagement with overlays active.
