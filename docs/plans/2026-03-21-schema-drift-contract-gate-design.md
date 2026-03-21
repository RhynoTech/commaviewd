# Schema Drift Contract Gate Design (commaviewd)

## Context
The current `commaviewd` schema drift check is file-level only: it compares tracked Cap'n Proto file hashes in `android-schema/manifest.json` against upstream checkouts. That was the right move for Android schema snapshot/codegen hardening, but it is too coarse for runtime telemetry contract visibility. It misses field and enum changes inside already-tracked services, which is exactly where silent drift gets interesting.

Rhyno wants the next gate to be explicit, reviewable, and strict enough to catch real upstream movement without depending on private Android repo access from public CI.

## Approved Decisions
1. **Granularity:** field/enum-level drift detection, not file-level only.
2. **Source of truth:** checked-in, handwritten contract manifest in `commaviewd`.
3. **Noise control:** checked-in, reviewed ignore manifest in `commaviewd`.
4. **Default policy:** fail closed. New unignored drift breaks CI.
5. **Coverage:** track **all services** initially for maximum visibility; suppress non-value items explicitly through the ignore manifest.
6. **Upstream baselines:** run separate checks for **openpilot** and **sunnypilot**; do not merge them into a superset.
7. **Implementation style:** structured schema diff with a normalized schema graph; no raw text diff hacks.

## Rejected Alternatives
### File/service-level only
Too blunt. It catches file replacement but misses additive/removal/type drift inside existing services.

### Full upstream schema diff for everything with no contract layer
Strict as hell, noisy as hell. It would produce a wall of churn without telling us what CommaView actually considers relevant.

### Generated-only contract manifest
Tempting, but too magical. Rhyno wants the checked-in contract to stay explicit and reviewable.

## Target Architecture
### Checked-in artifacts
`commaviewd` owns three schema-control artifacts:
- `android-schema/manifest.json` — existing file-hash snapshot, retained for coarse source provenance.
- `android-schema/contract-manifest.json` — handwritten service/field/enum contract.
- `android-schema/ignore-manifest.json` — reviewed suppressions with reasons and scope.

### Drift checker shape
Keep the existing shell entrypoint for workflow stability, but move the real logic into a Python helper that:
1. loads the checked-in contract + ignore manifests
2. parses upstream Cap'n Proto sources from a target checkout
3. normalizes the relevant schema surface into a stable graph
4. diffs the upstream graph against the checked-in contract
5. subtracts explicitly ignored drift
6. emits a machine-readable report and a human-readable summary
7. exits non-zero when any unignored drift remains in fail-closed mode

### Normalized comparison model
Each tracked item should be reducible to a stable key that survives formatting churn:
- upstream label (`openpilot` / `sunnypilot`)
- source file path
- service / type name
- field name
- field ordinal
- field type signature
- enum name
- enum value name + ordinal
- union membership where relevant

Raw text diff is intentionally out. The gate should reason about schema meaning, not whitespace.

## Contract and Ignore Model
### Contract manifest
The contract manifest is handwritten and checked in. It should explicitly enumerate the service surface we care about, even though helper tooling may generate candidate JSON for review.

Suggested shape:
- top-level metadata (`version`, `generatedFrom`, `notes`)
- explicit service entries keyed by service/type name
- per-service file path
- per-service tracked fields with ordinals and normalized type signatures
- per-service tracked enums with explicit values

Because Rhyno chose **all services** for day one, the initial contract will be broad. The point is visibility first, pruning second.

### Ignore manifest
The ignore manifest suppresses noise deliberately, not accidentally. Each ignore entry should include:
- upstream scope (`openpilot`, `sunnypilot`, or `both`)
- service/type
- symbol (`field`, `enum`, `service`)
- drift class (`added`, `removed`, `type-changed`, etc.)
- rationale
- optional expiry / review note

If we decide a service or symbol adds no value, it moves into the ignore path with a reason. It does not disappear into undocumented mush.

## CI and Runtime Flow
CI runs the structured drift gate independently against:
1. an `openpilot` checkout
2. a `sunnypilot` checkout

Both runs use the same checked-in contract and ignore manifests, but produce separate summaries and reports. If either fork has unignored drift, the job fails.

Expected outputs:
- concise summary in the GitHub Actions step summary
- machine-readable JSON artifact in `dist/`
- clear failure list including upstream label, service, symbol, drift class, and file path

## Failure Policy
Fail closed means:
- newly added field in a tracked service → fail
- removed field in a tracked service → fail
- field type/ordinal change in a tracked service → fail
- enum value add/remove/rename in a tracked enum → fail
- newly seen service in the all-services contract bootstrap path → fail unless explicitly ignored or added to contract

The only ways to make CI green again are:
1. update the contract manifest
2. add a reviewed ignore entry
3. fix the checker if it is wrong

## Rollout Plan
1. Implement the structured checker with fixture-backed tests.
2. Add a bootstrap/report mode that prints a candidate contract and candidate ignores without mutating checked-in files.
3. Build the initial handwritten all-services contract from reviewed bootstrap output.
4. Run locally against current openpilot and sunnypilot trees to measure blast radius.
5. Tighten the ignore manifest until the remaining failures are meaningful.
6. Flip CI workflows from warn-only to enforced fail-closed mode.

## Testing Strategy
### Unit tests
Fixture-driven Python tests should cover:
- additive field drift
- removed field drift
- type change drift
- enum value drift
- ignored vs unignored drift
- service add/remove drift
- normalized key stability for reordered source text

### Contract tests
Shell/Python contract tests should verify:
- required manifest files exist
- checker supports `--help`
- checker fails when manifests are missing or malformed
- checker writes the expected JSON report

### Integration tests
Run the checker against pinned openpilot/sunnypilot snapshots and confirm:
- zero false positives for the chosen contract/ignore set
- intentional fixture drift still fails
- CI summaries stay readable instead of vomiting logs everywhere

## Success Criteria
The design is successful when:
- upstream field/enum drift in either fork yields a precise, reviewable CI failure
- low-value drift can be muted only through a checked-in ignore manifest with reasons
- public CI does not require private Android repo access
- the contract stays explicit and human-reviewable
- the gate tells us what changed, not just that some file hash moved
