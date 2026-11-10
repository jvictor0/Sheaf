# Sheaf parameter behavior in Lean

This model adopts Smart Grid's separation of observable encoder behavior from
implementation strategy. `Sheaf/Schedule.lean` is adapted from its GPL-3.0 schedule
proof at the revision pinned in `requirements.json`; `COPYING.smart-grid` retains
the license. The remaining model describes Sheaf's own normalized parameter API.

Run `make formal-test` with Lean/Lake 4.32.0 on PATH and a C++20 compiler.
The real-valued filter proof uses Mathlib v4.32.0, pinned with its transitive
revisions in `lake-manifest.json`. Initial setup uses `lake update` and Mathlib's
proof cache; dependencies live in this checkout's ignored `.lake/packages`.
Veil is not required. The gate builds all proofs, checks reviewed source
fingerprints and freshly exported C++ filter coefficients, then reruns the
transitive axiom audit even on a cached build. Only Lean's standard `propext`, `Classical.choice`, and `Quot.sound`
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

The parameter model abstracts floats and external warp functions. It does not
prove IEEE rounding, the output FIR frequency response, the actual graph
builder, weighted absolute-edit projection solver, event recording/replay, gesture
edit allocation, allocator ownership, hardware/UI dispatch, performance or the
entire C++ implementation. Source filtering and app integration are native-tested.
Model-level leaf/reset/clear operations specify their postconditions, not their
full UI command routing. The manifest states each correspondence boundary.

At 48 kHz host cadence, the integrated Froggers engine runs at 192 kHz. Other
host rates retain the ratio. Targets and the Fuego/UI seam advance at host cadence;
Low-pass-filtered modulation sources and audio DSP advance at engine cadence. Root
publication weights are 1/4, 1/2, 3/4 and 1. Filter history persists across callbacks and holds exactly while source advancement is disabled by stopped transport.

The Froggers integration models density quartiles over the number of target
parameters receiving modulation, independently of source count per selected target.
Coverage proofs establish bounds and the four-preimage marginal count mapping.
Source weights peak at two eligible sources, halving at each step away from the
mode; native tests enumerate every integer entropy ticket and verify those ratios.
Separate proofs specify geometric depth magnitudes, finite probability mass and
one-way signs. Uniform independent RNG inputs are assumptions. Native tests cover
target selection, actual sampler weights, floating-point curve inversion, held
gestures, recursive scope and storage lifecycle. The app-specific correspondence
fingerprints `../../../../app/FroggersModulation.hpp`, so this audit runs within
Froggers. These behavioral proofs do not establish audio-thread deadline safety.

The additional real-valued [alias-rejection model](ALIASING.md) proves the fixed
modulation-source filter's continuous-band frequency-response bounds using exact
binary32 coefficient embeddings. It also proves the 47 kHz to 1 kHz folding
identity under four-to-one sampling. Its scope excludes per-sample float error,
startup transients, nonlinear DSP and the separate output FIR.

The [state/arithmetic extension](STATE_AND_ROUNDING.md) adds full biquad memory,
concrete stability and initial-history decay, nearest-even binary32/binary64
rounding with FTZ/DAZ, accumulated disturbances, the actual 287-tap output FIR,
and nonlinear cell/jump contracts. Its guards and compiler-correspondence limits
are explicit; it does not claim whole-instrument alias freedom.
