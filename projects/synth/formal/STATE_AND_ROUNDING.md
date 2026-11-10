# State, arithmetic, and composition contracts

These models complement the fixed-frequency bounds in [ALIASING.md](ALIASING.md).
They use the implementation's freshly exported binary32 coefficients and taps.
The claims are mathematical contracts with explicit finite-operand and state
preconditions. They are not a compiler/hardware refinement proof or a proof
that every possible Froggers patch stays inside every arithmetic guard.

## Biquad state and startup

`Sheaf/FilterState.lean` models all four direct-form-I memory values: two input
samples and two output samples. Its full update matches the actual recurrence.
Two executions receiving the same two new samples have identical input histories;
their remaining output difference follows a second-order recurrence, with
rounding/upstream differences represented as a disturbance.

For each of the four actual exported sections, the kernel verifies a positive
energy form

    E(x,y) = x² + a1*x*y + a2*y²

with `(x²+y²)/1000 ≤ E ≤ 2*(x²+y²)` and `0 ≤ a2 ≤ 4/5`.
An unforced update satisfies **E(next,x) = a2*E(x,y)** exactly. Thus initial output
history is forgotten exponentially, energy tends to zero, and after 512 aligned,
unforced samples energy is at most `10^-49` of its initial value.

This 512-sample theorem is about a single section's homogeneous error, after
input histories agree. It is not a claim that the whole rounded cascade becomes
exactly settled after its 512 priming calls. The native suite separately checks
actual four-section DC priming against the rounded-coefficient steady response.

For arbitrary, possibly adversarial disturbances `|d[n]| ≤ η`, the model proves

    E[n] ≤ 0.9^n*E[0] + 90*η²*(1-0.9^n)
    error[n]² ≤ 1000*E[n].

No zero-mean or independence assumption is used. Upstream input differences enter
through the three feedforward coefficients; a separate theorem adds those to the
local arithmetic residual. Rounded executions can retain a nonzero error floor;
the unforced exact-real decay theorem does not erase rounding or limit cycles.

## Finite IEEE format model

`Sheaf/Rounding.lean` defines nearest-even integer selection, adaptive binary32
spacing (23 fraction bits, subnormal quantum `2^-149`) and binary64 spacing
(52 fraction bits, subnormal quantum `2^-1074`). It proves half-ULP errors.
The finite regional corollaries are:

- exact binary32 operation input magnitude ≤ 8: error ≤ `2^-21`;
- exact binary64 operation input magnitude ≤ 16: error ≤ `2^-49`.

These guards avoid overflow. The model excludes NaN/infinity operands and
requires round-to-nearest, ties-to-even. Native checks verify IEEE formats,
`FE_TONEAREST`, ties, finite-region residuals and the actual state-copy order.
A separate ARM64 test changes FPCR to demonstrate both real output FTZ and input
DAZ behavior, then restores the complete floating-point environment.
Compiler instruction selection/FMA contraction and hardware conformance remain
implementation assumptions checked natively, not kernel-proved machine-code facts.

The source biquad's five products and four add/subtract operations have an
explicit operation trace. Its local residual is bounded by nine primitive-error
allowances. Flush-to-zero (FTZ) and denormals-are-zero (DAZ) are modeled too:
flushing changes a value by at most the smallest normal, `2^-126` for binary32.
The actual source coefficients are proved normal and have magnitude at most two.
With FTZ/DAZ, the nine-operation budget is

    9*2^-21 + 19*2^-126.

The VST uses `juce::ScopedNoDenormals`; gradual and FTZ models are both available.
The residual bound feeds the state-energy theorem, giving an accumulated-error
bound, not just a one-operation estimate. Its uniform constants are conservative:
they do not upgrade the earlier exact-real 60 dB frequency theorem into a 60 dB
claim for every rounded trajectory or nonlinear patch.

## Actual output FIR and final clamp

The gate also exports all **287 stored binary32 FIR taps**, including the
constructor's double-to-float conversion. `Sheaf/OutputFir.lean` proves:

- coefficient L1 gain ≤ **9/4** (the actual sum is about 2.1717674);
- all initial history disappears after 287 common engine samples; the next
  matching four-to-one emission has the same history-dependent value;
- input-error bound B gives exact-convolution output error ≤ `(9/4)*B`;
- the final full-scale clamp cannot increase a value difference;
- DAZ input flushing adds at most `(9/4)*2^-126` to that tail error;
- the actual mixed-precision pattern—binary32 products, binary64 accumulation,
  binary32 output—has error at most
  `(N+1)/1048576 + N/281474976710656`, under its explicit product (8),
  accumulator (16) and final rounding-input (8) guards. With N=287 this is
  below **1/3000**. Both gradual and FTZ rounding modes are modeled.

The FIR model is a finite convolution; native tests separately bind tap order,
phase, ring-history flushing and actual arithmetic residuals. The model does
not prove the C++ ring-buffer implementation by refinement. These are time-domain
error/history guarantees, not a new FIR stopband or whole-synth spectral proof.

## Fuego: the nonlinear contract matters

`Sheaf/NonlinearBoundary.lean` models the actual integer scramble at maximum
Fuego and row zero, with a real floor/remainder input extension. It proves that
code 0 maps to 0 while code 1 maps to 22. At the first input cell boundary,
inputs separated by ε can have outputs separated by `21/255+ε`.
The real extension therefore has no finite global Lipschitz gain. This statement
is not about the finite binary32 domain: that domain has a finite, very large
worst-case gain. A native adjacent-float test measures input difference about
`4.6566e-10` and output difference about `0.08235294` (>100 million-fold gain).

There is a useful conditional contract. The generic floor/remainder cell warp
is `input + cellOffset`. With the same code and input cell it preserves an error
exactly. On a cell/control-code crossing, its error bound adds the explicit
cell-offset jump. A two-stage theorem propagates global/local warp errors with
both jump terms. Native tests check within-cell behavior across all nine masks,
all 16 supported rows and all 255 interior cells, with floating-point tolerance.

Consequently the source filter's small linear error/attenuation bound cannot
simply be transported through Fuego with gain one. A whole-instrument claim
needs the cell/code guards or explicit jump budgets, plus contracts for its
other DSP stages. Final clipping is nonexpansive in value error but may still
create harmonics; that property does not establish spectral transparency.

## Negative controls and remaining boundary

An unstable exported-section coefficient fails the stability certificate. A
one-bit FIR snapshot mismatch fails the native coefficient gate. A native
FE_TOWARDZERO run fails the nearest-even test. The Fuego bypass is the positive
control for its discontinuity test.

No universal whole-synth alias-free guarantee is asserted. Oscillator generation,
time-varying effects, nonlinear spectral behavior, arbitrary nonfinite inputs,
compiler lowering, and deadlines are not verified by these contracts. The model
now makes the state/arithmetic/composition assumptions explicit rather than
silently assuming all stages inherit a linear filter's guarantee.
