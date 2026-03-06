# Bridge Phase 1 Parity Verification (COM-47)

Date: 2026-03-06
Scope: Verify Phase 1 modular extraction preserved bridge runtime contract.

## Contract checks (source)
- Message types unchanged in `bridge/cpp/commaview-bridge.cc`:
  - `MSG_VIDEO = 0x01`
  - `MSG_META = 0x02`
  - `MSG_CONTROL = 0x03`
- Stream ports unchanged:
  - `PORT_ROAD = 8200`
  - `PORT_WIDE = 8201`
  - `PORT_DRIVER = 8202`

## Build parity
Command:
```bash
cd /home/pear/CommaView/bridge/cpp
OP_ROOT=/home/pear/openpilot-src ./build-ubuntu.sh
```

Result:
- Host binary built: `commaview-bridge-host`
- Deploy binary built: `commaview-bridge-aarch64`
- Bundled libs built/copied with expected ABI:
  - `libcapnp-0.8.0.so`
  - `libkj-0.8.0.so`
- ARM binary RUNPATH remains `$ORIGIN/lib`

## Runtime startup smoke
Command:
```bash
timeout 4s ./commaview-bridge-host --video-only
```

Observed:
- Bridge banner printed with expected flags.
- Bind failed on `8200` due `Address already in use` on dev host (existing listener), confirming startup path reaches bind stage.

## Phase 1 extraction summary
- COM-45 extracted `net` module (framing/socket)
- COM-46 extracted `control`, `video`, `telemetry` modules
- `commaview-bridge.cc` is now orchestration-centric and delegates module behavior.

Conclusion: Phase 1 refactor preserves protocol/port contract and build/deploy shape.
