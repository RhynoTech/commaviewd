# Raw Telemetry Hard-Cutover Design (CommaViewD)

## Context
In-car testing shows stability returns when carState telemetry is disabled (--telem-safe-no-car). Current bridge behavior still decodes telemetry on comma, including drained samples.

## Decisions (Approved)
1. Hard cutover to raw-only telemetry transport on comma now.
2. Focus commaviewd runtime only in this phase.
3. Do not touch Android code in this session until Rhyno confirms status of the parallel UI/UX session.
4. Produce two AI-facing docs: short operator README plus deep troubleshooting reference.

## Target Architecture
### Comma bridge
- Keep video path unchanged.
- Telemetry path becomes transport-only: subscribe, drain queue to latest, forward raw envelope (MSG_META_RAW).
- No telemetry JSON construction and no typed telemetry encode work in normal runtime path.
- Keep one internal emergency rollback switch (not user-facing).

### Android app
- Android becomes sole decode and render layer for telemetry overlays and fallback UI.
- Android consumes raw envelopes and decodes required messages locally.

## Scope
In scope: commaviewd bridge raw-only refactor, runtime defaults, docs, and verification updates.
Out of scope: Android app code changes in this session.

## Verification
1. commaviewd unit and contract tests pass.
2. Runtime logs show raw-only telemetry path active.
3. In-car validation with carState enabled shows stable engagement.
4. Release is published via GitHub CI tag workflow.

## Deliverables
- docs/ai/telemetry-raw-only-readme.md
- docs/ai/telemetry-raw-only-deep-dive.md
- docs/plans implementation plan for execution

## Coordination Constraint
Before Android changes, check with Rhyno to avoid stepping on the parallel UI/UX refactor session.
