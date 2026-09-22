# comma4 Level-2 Engagement Gate

Date: 2026-09-22

## Purpose

Exercise the real openpilot engagement and control pipeline on the offroad comma4 without attaching a vehicle. The gate runs in isolated msgq and Params namespaces so the production manager remains online and cannot consume the simulated traffic.

## Test boundary

The harness runs the real manager, `card`, `selfdrived`, `plannerd`, `radard`, and `controlsd`. It supplies deterministic Honda Civic 2022 CAN/Panda state, route-derived `modelV2`, driver-monitoring state, calibration, motion, and parameter messages. It sends a SET command and requires five continuous seconds of active controls.

Assertions:

- engagement becomes available and then active;
- `selfdriveState.active`, `controlsState`, and enabled `carControl` remain above 70 Hz;
- no `commIssue`, `controlsMismatch`, `processNotRunning`, CAN, or relay faults;
- no required process becomes unhealthy after engagement;
- CommaView's isolated worker thread emits framed payloads.

This proves the software engagement sequence and sustained controls under exporter load. It does **not** prove a vehicle-specific physical CAN topology, panda safety response against real ECUs, or actuator behavior.

## Final comma4 results

| Mode | Engaged | Active updates | Controls updates | Enabled carControl | Export frames | Critical faults | Result |
|---|---:|---:|---:|---:|---:|---:|---:|
| Baseline | yes | 480 | 428 | 383 | 0 | 0 | PASS |
| `orjson` thread | yes | 484 | 405 | 375 | 232 | 0 | PASS |

Evidence on device:

- `/data/commaview-bench/results/level2-baseline.json` — SHA-256 `28642fcbaad49f6af8399504f722af76d3161f09f0b33711b6d50eae7982134e`
- `/data/commaview-bench/results/level2-thread.json` — SHA-256 `208f314b2fba8ff761a3fe7e08b4c53bffbabb6c3e15ba277f1319c8a2a25b7f`

## Reproduction

Run only while the production device is offroad:

```bash
/data/commaview-bench/bin/comma4-level2-engagement.sh
```
