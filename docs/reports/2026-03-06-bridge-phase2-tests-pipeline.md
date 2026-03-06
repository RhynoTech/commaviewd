# Bridge Phase 2 — Unit Tests + Verification Pipeline (COM-49)

Date: 2026-03-06

## What was added
- `bridge/cpp/run-unit-tests.sh`
  - compiles + runs bridge unit tests for framing, control, telemetry JSON shaping
- `bridge/cpp/run-verification.sh`
  - full phase-2 pipeline: reproducible build + unit tests
- Unit tests:
  - `bridge/cpp/tests/test_net_framing.cpp`
  - `bridge/cpp/tests/test_control_policy.cpp`
  - `bridge/cpp/tests/test_telemetry_json.cpp`
  - `bridge/cpp/tests/unit_tests_pipeline_test.sh`

## Verification commands
```bash
OP_ROOT=/home/pear/openpilot-src bridge/cpp/run-unit-tests.sh
OP_ROOT=/home/pear/openpilot-src bridge/cpp/run-verification.sh
```

## Result
- PASS: all unit tests
- PASS: verification pipeline completes end-to-end
