# Bridge Phase 2 — Reproducible Build Verification (COM-48)

Date: 2026-03-06

## What was added
- `bridge/cpp/reproducible-build.sh`
  - runs bridge build twice with fixed `SOURCE_DATE_EPOCH`
  - compares SHA256 for host + aarch64 artifacts
  - records toolchain + checksums to `bridge/cpp/reproducible-build-manifest.json`
- `bridge/cpp/tests/reproducible_build_test.sh`
  - verifies script is executable and has `--help`

## Verification commands
```bash
OP_ROOT=/home/pear/openpilot-src bridge/cpp/reproducible-build.sh
```

## Result
- PASS: host and arm artifact checksums matched across two successive builds
- Manifest generated with tool versions and artifact hashes
