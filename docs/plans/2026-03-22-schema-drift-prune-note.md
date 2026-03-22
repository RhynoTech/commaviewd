# Schema Drift Prune Note

- The old broad Android schema drift gate was intentionally removed from `commaviewd` on 2026-03-22.
- Runtime CI and canaries now prove **HUD-lite reality** instead: patch applicability against current upstream refs, plus the existing runtime verification pipeline.
- `commaviewd` no longer carries `android-schema/`, schema drift scripts, or schema drift artifacts.
- `CommaView` still owns the Android schema snapshot/codegen guards (`verify-schema-snapshot.sh`, `verify-no-committed-generated-bindings.sh`, `verify-generated-bindings-reproducible.sh`).
- Do **not** reintroduce broad raw-telemetry schema drift checks in the runtime repo unless product reality changes again.
