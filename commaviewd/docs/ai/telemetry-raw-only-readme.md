# Telemetry Raw-Only Mode (Operator Quick Reference)

## What is live now
- Comma bridge runs telemetry in raw-forward mode by default.
- Comma keeps latest sample per service and forwards raw envelopes.
- No per-sample telemetry JSON or typed decode work should run on comma in normal mode.

## Why
- Reduce comma-side decode pressure, especially for high-rate carState.
- Improve openpilot engagement stability while restoring full telemetry availability.

## Expected runtime signals
- Bridge startup log should include a raw-only marker.
- Telemetry counters should show raw emits and low/no JSON emits in default mode.

## Emergency rollback
- Internal-only rollback switch can temporarily re-enable legacy decode path for field recovery.
- Do not expose rollback in normal UI flows.

## Validation checklist
1. Confirm raw-only startup marker in bridge log.
2. Confirm carState is subscribed and forwarded.
3. Confirm stable drive engagement with overlays active.
