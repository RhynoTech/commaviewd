# Schema Drift Prune for HUD-lite Runtime Design

## Context
`commaviewd` already hard-cut runtime telemetry over to HUD-lite. The old Android schema drift gate in this public repo now protects a dead compatibility story: broad Cap'n Proto drift against upstream raw telemetry services that the runtime no longer consumes.

## Options considered
1. **Keep the broad drift gate and trim only CI noise.** Lowest short-term churn, but it keeps dead compatibility logic, dead manifests, and dead failure modes alive.
2. **Full prune and re-anchor CI on HUD-lite reality.** Remove the schema drift scripts/manifests/tests/workflow steps from `commaviewd`; keep only guards that prove HUD-lite patch applicability, patch health, and runtime verification against current canary refs. **Recommended / approved.**
3. **Move the drift gate into `CommaView`.** Keeps schema awareness near Android, but it still solves the wrong runtime problem and would duplicate the existing app snapshot/codegen guards.

## Approved design
- Delete the old `commaviewd` schema drift machinery entirely:
  - `android-schema/`
  - `scripts/check-android-schema-drift.sh`
  - `scripts/check_android_schema_drift.py`
  - `commaviewd/tests/schema_contract_*`
  - workflow wiring and artifact references that mention Android schema drift
- Replace workflow coverage with HUD-lite-specific checks:
  - CI and canaries should prove the HUD-lite patch can apply/verify against the checked-out upstream ref
  - summaries/artifacts should surface HUD-lite status instead of schema drift artifacts
- Keep `CommaView` focused on Android-owned guards only:
  - schema snapshot integrity
  - no committed generated bindings
  - reproducible codegen
- Add short prune notes so future changes do not resurrect the dead runtime schema gate.

## Success criteria
- No tracked `commaviewd` runtime workflow/test/script references remain for Android schema drift.
- `commaviewd` CI/canaries validate HUD-lite patch applicability/health against real upstream refs.
- `CommaView` still has its schema snapshot/codegen guards and gets a short note clarifying the runtime-side prune.
