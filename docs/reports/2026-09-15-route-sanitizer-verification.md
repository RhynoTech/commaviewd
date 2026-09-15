# Route sanitizer verification — 2026-09-15

Tool: `tools/contribute/sanitize_route_for_contribution.py`
Test route: `/home/rhyno/Development/openpilot-routes/2026-05-30-latest-comma`
(real sunnypilot MICI capture, 22 segments: 117-120, 140-157)
Schema used for verification: `/home/rhyno/.cache/ci-ref-checkouts/sunnypilot-release-mici`
Known dongle ID in source route: `d1b810b2f0bc811f`
Known VIN in source route: `2T3DFREV7GW411281`

## Whitelist reconciliation

The brief's whitelist was reconciled against the authoritative export templates
(`comma/src/commaview_export.openpilot.py`, `comma/src/commaview_export.sunnypilot.py`).
Three services the templates read were missing from the brief and were added:

- `liveParameters` — both flavors (`sm["liveParameters"]`, roll/steering payloads, recv_frame gating)
- `onroadEvents` — both flavors (alert/event list payload)
- `longitudinalPlanSP` — **sunnypilot only** (speed limit resolver)

Per the brief, the union across flavors is preserved. `driverStateV2` and
`driverMonitoringState` are opt-in and excluded by default.

## 1. Run against real segments

```
OPENPILOT_ROOT=/home/rhyno/.cache/ci-ref-checkouts/sunnypilot-release-mici \
  python3 tools/contribute/sanitize_route_for_contribution.py \
  /home/rhyno/Development/openpilot-routes/2026-05-30-latest-comma \
  --segments 117-120 -o /tmp/cv-donate --force
```

Endpoint protection fired without being asked:

```
Refusing the route's first and last segment by default: [117]
```

Result: segments 118, 119, 120 exported. 17 services kept, 42 dropped.
Build identity resolved from rlog `initData` build fields only:
flavor `sunnypilot`, remote `https://github.com/sunnypilot/openpilot.git`,
branch `staging`, commit `fad75e97cf79d315c2b6cd745f52428fe55ddbdf`,
version `2026.001.000`, device model `mici`, working tree `clean`.
No dongle ID, serial or account identifier emitted.

## 2-4. Re-read sanitized output with LogReader

```
[1] services present: 17
[2] BANNED services present: NONE  <-- PASS
[3] non-whitelisted present: NONE  <-- PASS
[4] preserved intact, deep-parsed fields ok on 25200 messages
[5] VIN values in bundle: {'2T3DFREV7GWXXXXXX'} -> MASKED PASS
```

Banned set checked explicitly: `gpsLocation`, `gpsLocationExternal`,
`liveLocationKalman`, `liveLocationKalmanDEPRECATED`, `navInstruction`, `navRoute`,
`initData`, `logMessage`, `androidLog`, `errorLogMessage`, `ubloxGnss`, `ubloxRaw`,
`livePose`, `thumbnail`, `driverCameraState`, `driverEncodeIdx`, `driverStateV2`,
`driverMonitoringState`, `liveMapDataSP`, `can`, `sendcan`, `sentinel` — all absent.

Check [3] is stronger than [2]: it asserts the output contains *nothing* outside the
whitelist, so a service nobody thought to ban still cannot appear.

Check [4] deep-parsed `modelV2.position.x`, `carState.vEgo` and `radarState.leadOne.dRel`
across 25200 messages without error — preserved services survive intact and parseable.

## 5. Dongle ID byte grep

```
-- raw bytes --            0 hits in raw bundle bytes
-- decompressed rlogs --   total hits in decompressed rlogs: 0
-- sanity: same grep on SOURCE route (must be >0) --   182
-- unmasked VIN hits in bundle --                      0
```

The source segment contains 182 occurrences of `d1b810b2f0bc811f`; the sanitized
bundle contains zero, compressed and decompressed. The sanity line proves the grep
itself works.

## Flag behavior

- `--include-driver-monitoring` → `driverStateV2` (1199) and `driverMonitoringState`
  (1199) appear; service count goes 17 → 19. Off by default.
- `--include-video road,wide,qcamera` → `fcamera.hevc`, `ecamera.hevc`, `qcamera.ts`
  copied; receipt prints the "cannot be anonymized" warning. Off by default.
- `--include-video driver` and `--include-video dcamera` → both rejected:
  `ERROR: unknown --include-video value(s): [...]. Valid: road, wide, qcamera`
- `find /tmp/cv-flags -name '*dcamera*'` → 0 files, including in the full opt-in run.
- `--tar` produced an 80.2 MiB archive for the video-inclusive single-segment bundle.

## Defect found and fixed during verification

The first run resolved `openpilot` from an already-importable vanilla checkout and
ignored `OPENPILOT_ROOT`. Under the vanilla schema, sunnypilot services decode as
`customReserved0..9`, so `longitudinalPlanSP` was silently dropped instead of preserved.
`bootstrap_openpilot()` now gives an explicit `OPENPILOT_ROOT` precedence over ambient
imports, and `DONATE_A_DRIVE.md` warns users to match the checkout to their build.
Re-verified after the fix: `longitudinalPlanSP` 3600 messages preserved.
