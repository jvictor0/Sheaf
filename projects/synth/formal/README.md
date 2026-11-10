# Sheaf parameter behavior in Lean

This model adopts Smart Grid's separation of observable encoder behavior from
implementation strategy. `Sheaf/Schedule.lean` is adapted from its GPL-3.0 schedule
proof at the revision pinned in `requirements.json`; `COPYING.smart-grid` retains
the license. The remaining model describes Sheaf's own normalized parameter API.

Run `make formal-test` with Lean/Lake 4.32.0 on PATH. No Mathlib, Veil, network
fetch or global installation is required. The gate builds all proofs, checks
reviewed source fingerprints, then reruns the transitive axiom audit even on a
cached build. Only Lean's standard `propext`, `Classical.choice`, and `Quot.sound`
axioms are permitted. There are no admitted proofs or trusted SMT results.

`requirements.json` is the reviewed source correspondence manifest. Hashes are
drift alarms, not proof of equivalence. After a covered source edit, review its
contracts and update model/tests as needed before updating a fingerprint.

The model includes arbitrary scene/voice/gesture counts, normalized leaf edits,
configured reset defaults with immediate current reset, signed crossfade sums, attenuverter rest conventions,
one-way disconnected amounts and blend-mode override, optional gesture slots,
view-pinned storage, conditional quiescence, the application Fuego seam and the
four-to-one control/engine publication contract. These extend the fixed scene/lane
Smart Grid model and its single crossfade law. The schedule theorem is retained
as a reusable result; this integration uses Sheaf's recursive evaluator with reuse of unchanged scalar depth leaves.

The model quantifies over arbitrary finite counts; the native gesture selector is limited to 64 slots by its uint64 mask.

The rational model abstracts floats and external warp functions. It does not
prove IEEE rounding, source IIR/output FIR frequency response, the actual graph
builder, weighted absolute-edit projection solver, event recording/replay, gesture
edit allocation, allocator ownership, hardware/UI dispatch, performance or the
entire C++ implementation. Source filtering and app integration are native-tested.
Model-level leaf/reset/clear operations specify their postconditions, not their
full UI command routing. The manifest states each correspondence boundary.

At 48 kHz host cadence, the integrated Froggers engine runs at 192 kHz. Other
host rates retain the ratio. Targets and the Fuego/UI seam advance at host cadence;
Low-pass-filtered modulation sources and audio DSP advance at engine cadence. Root
publication weights are 1/4, 1/2, 3/4 and 1. Filter history persists across callbacks and holds exactly while source advancement is disabled by stopped transport.
