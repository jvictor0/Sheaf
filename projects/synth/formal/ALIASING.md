# Fixed converter response certificate

The gate is `make formal-test`, using the locked Mathlib/Lean 4.32 toolchain.
It freshly exports all **30 binary32 coefficients** from the actual six-section
`RateConversionLowPass` in `DspRateConversion.hpp`, checks the JSON and exact-real
Lean embeddings, regenerates and compares the Bernstein proof text, builds all
Lean dependencies and audits every theorem's axioms.

`ResponseCore.lean` defines one six-section list. It proves the direct complex
magnitude expression equals its cosine polynomial, every denominator is strictly
positive on [-1,1], the aggregate numerator is nonnegative, and the product of
six section quotients is the aggregate numerator divided by aggregate denominator.
`SourceFilterCertificate.lean` is generated from the exported JSON bits using
Python's exact rational arithmetic, without SciPy. Lean's kernel checks both
aggregate polynomial identities and every Bernstein expansion/nonnegative witness.
The assembly proves the entire closed cosine intervals, including every shared
leaf boundary. Python is a proof-text generator, not a trusted numerical oracle.

`Aliasing.lean` proves the trigonometric endpoint inclusions with exact angle and
square-root identities and rational square bounds. For normalized frequency
`r = frequency / engineRate`, the complete closed bands satisfy:

- `0 ≤ r ≤ 5/48`: power gain between `981/1000` and `10001/10000`;
- `1/8 ≤ r ≤ 1/2`: power gain between zero and `981/1000000000000`;
- every stop/pass pair: power ratio at most `1/1000000000` (90 dB).

At a 192 kHz engine the bands are 0..20 kHz and 24..96 kHz. Other engine rates
scale the physical edges. The 20..24 kHz transition band has no such bound.
The stronger absolute stop bound includes the pass-floor factor; dividing an
absolute `10^-9` bound by `.981` would not prove the stated relative ratio.
The retained 47 kHz folding identity explains four-to-one matrix downsampling.

These are exact-real steady-state transfer bounds of the stored coefficients.
They do not establish rounded trajectories, startup/Reset convergence, transient
suppression, arbitrary-signal energy bounds, root FOH image levels, limiting
harmonics, the output FIR stopband, deadlines, or nonlinear whole-instrument
alias freedom. Scalar and batch native kernels share the coefficient table and
section-step expression; the model makes no C++/compiler/hardware refinement
claim. Native response, independent scalar/batch, bridge, limiting and callback
checks remain separate evidence.

[STATE_AND_ROUNDING.md](STATE_AND_ROUNDING.md) describes the retained conditional
state algebra and finite arithmetic contracts. Reviewed source fingerprints and
live anchors detect correspondence drift but do not prove arbitrary C++ control
flow refines Lean. `--write` refreshes snapshots/certificates only after review;
the complete proof and coefficient gate must still pass.

Root publication additionally restores an exact normalized matrix target after
4096 unchanged-target engine samples, only within binary32 `2e-5f` of the
unconditionally advanced raw filter. Exact phase-zero target changes reset age;
Configure resets age to zero, and increment precedes eligibility. The rational
publication model bounds correction relative to clamped filtered publication by
1/50000 pointwise. This nonlinear policy has no universal 90 dB spectral claim;
the unchanged linear certificate excludes settlement and limiting. Arbitrary
histories are not guaranteed eligible by sample4096. Native boundary/reset,
live-history and moving-image tests separately exercise the implementation.
