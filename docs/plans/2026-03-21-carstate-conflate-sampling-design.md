# carState Conflated Sampling Design

## Context
Road testing on comma4/mici shows a repeatable failure pattern: enabling CommaView runtime telemetry for `carState` causes openpilot/sunnypilot engagement instability (`commIssue` / immediate disengage), while the previously validated safe bundle without `carState` remains stable. Additional March 21 testing narrowed this further:

- `modelV2` can be reduced/off without reproducing the failure.
- `carState` can reproduce the failure by itself, even when emitted at `1 Hz`.
- Existing sampled mode reduces forwarded output to Android, but still performs active queue draining on the comma-side `carState` subscriber.

This points at the `carState` read strategy inside `commaviewd`, not just Android bandwidth.

## Constraints
- Keep the first fix attempt entirely inside `commaviewd`.
- Do not require maintained downstream openpilot/sunnypilot forks.
- Focus on the smallest possible experiment that can prove or falsify the hypothesis.
- Avoid broader telemetry changes until `carState` behavior is understood.

## Approved Scope
### Experiment scope
- `commaviewd` only
- `carState` only
- sampled mode only
- no release yet; local binary + on-device test only

### Explicit non-goals
- no openpilot/sunnypilot code changes
- no new lite publisher/service
- no changes to pass mode behavior
- no changes to other telemetry services
- no parity/HUD expansion work in this pass

## Hypothesis
The current sampled `carState` path is harmful because it actively drains a hot `100 Hz` queue in `commaviewd`. If sampled `carState` is changed to a true conflated/latest-only subscriber, the second subscriber may become cheap enough to stop triggering `commIssue` while still delivering low-rate raw `carState` samples to Android.

If this still fails, the conclusion is harsher: simply having a second `carState` subscriber is toxic, and subscriber strategy alone is not enough.

## Approved Architecture
### Current problematic behavior
Today, sampled services in `bridge_runtime.cc` use a non-conflated socket and manually drain backlog to keep the newest message. That means `commaviewd` still touches the hot upstream stream repeatedly even if Android only sees `1 Hz` or `2 Hz` output.

### Proposed behavior
For `carState` in sampled mode:
- create the subscriber with `conflate=true`
- stop manual backlog draining for that service
- on each sample emit tick, read at most one latest message
- forward that raw event unchanged through the existing raw meta envelope
- if no fresh message is available, emit nothing

All other services remain unchanged for the first experiment.

## Runtime Behavior Rules
### `carState` sampled mode
- uses a conflated subscriber
- never walks backlog
- never performs a drain loop
- forwards at most one latest raw message per sample interval

### `carState` pass mode
- unchanged
- still behaves exactly like existing pass-through raw forwarding

### Other telemetry services
- unchanged
- preserve existing sample/pass behavior until the `carState` experiment proves out

## Instrumentation Requirements
Keep runtime stats honest and comparable before/after:
- receive count
- emitted count
- sampled emit count
- drop/drain count
- send failure count
- max send stall

The implementation may reduce or eliminate drain counts for sampled `carState`, but should not hide the operational truth.

## Testing Strategy
### Pre-implementation proof target
A focused regression test should prove:
- sampled `carState` uses conflated/latest-only subscription semantics
- the old manual drain pattern is gone for the `carState` sampled path
- pass mode remains untouched
- non-`carState` sampled services remain untouched in this first pass

### On-device rollout
1. Build local binary
2. Deploy to comma4
3. Test `carState sample @ 1 Hz` with other telemetry disabled
4. If stable, re-test the previously safe bundle plus `carState sample`

## Success Criteria
This experiment is a success if:
- `carState sample @ 1 Hz` with live video no longer triggers `commIssue`
- `modelV2` and other previously safe services remain unaffected
- no upstream openpilot/sunnypilot modifications are required

## Failure Criteria
If this still reproduces engagement instability, conclude:
- any second `carState` subscriber is likely toxic on this platform/runtime
- subscriber behavior tuning is insufficient
- the next architecture must avoid a direct second `carState` subscriber entirely

## Notes from prior evidence
- March 4 testing established that `carState` socket creation appeared safe while the active read path was the suspected trigger. Source: `memory/2026-03-04.md#1`
- March 21 testing re-confirmed that masking must be real subscription behavior, not just “do not transmit,” and that `carState` sample mode should become a true latest-only sampler. Source: `memory/2026-03-21.md#24`
