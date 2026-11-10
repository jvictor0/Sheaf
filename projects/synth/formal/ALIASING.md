# Modulation-source alias rejection

`Sheaf/Aliasing.lean` extends the existing behavior model with real-valued filter
frequency-response proofs. It uses Mathlib v4.32.0 at the exact revision locked
in `lake-manifest.json`. The full gate remains `make formal-test`.

## Checked implementation correspondence

The configured source bridge runs four engine samples per control frame and
uses four low-pass biquads at cutoff `0.45 / 4` cycles per engine sample.
`scripts/check_source_filter_coefficients.py` compiles a fresh C++ exporter against
that actual filter type and cutoff. It compares all 20 binary32 coefficient bit
patterns to `source-filter-coefficients.json`, and verifies that
`Sheaf/SourceFilterCoefficients.lean` embeds precisely those floats as exact real
rationals. The audit fails before proof compilation if either correspondence
differs. `--write` refreshes the snapshot/definitions after intentional review;
the attenuation proofs must still succeed for the new values.

The bridge, coefficient exporter and DSP math/filter headers are also reviewed
source fingerprints. Fingerprints and coefficient exports are drift checks,
not a proof that arbitrary C++ control flow refines the Lean model.

## What is proved

For each real angular frequency, the full biquad numerator/denominator squared
magnitude expression equals its polynomial in `cos(omega)`. Closed exact
arithmetic certificates establish the bounds for each exported section; generic
proofs combine them into the four-section cascade. Real trigonometric inequalities
then connect the polynomial bounds to continuous frequency bands.

At an engine rate of **192000 Hz**:

- Every frequency from **47000 through 96000 Hz** has cascade power gain at most
  **1/4000000**.
- Every reference frequency from **0 through 1000 Hz** has cascade power gain
  at least **1/2**.
- Every high-band/reference pair therefore has power-gain ratio at most
  **1/1000000** and amplitude-gain ratio at most **1/1000**: at least **60 dB**
  relative rejection. The underlying power bounds leave additional margin.
- Sampling an unfiltered 47000 Hz sinusoid at every fourth 192000 Hz sample
  gives the inverted 1000 Hz sinusoid at 48000 Hz. This establishes why the
  high-frequency source must be filtered before matrix downsampling.

These are uniform band proofs, not conclusions from a finite frequency grid.
The concrete 47 kHz/1 kHz case is also proved separately. Other host/engine rates
would scale these physical frequency bands; the stated theorems use 192 kHz.

## Native checks and negative controls

The native parameter suite retains its actual binary32 time-domain check:
1 kHz passes and the 47 kHz output is below one thousandth of its RMS amplitude.
A separate response check samples 8193 points in each of the two proved bands
using the actual exported filter coefficients, and compares against the formal
power bounds. This is a numerical correspondence cross-check, separate from
the continuous proof.

Changing one expected coefficient bit is rejected by the export check. Doubling
the first stage's three numerator coefficients in a temporary proof copy preserves
its low-pass shape but violates its attenuation certificate; Lean rejects it.
No weakened theorem or new axiom is used to accept either control.

## State and arithmetic extension

[STATE_AND_ROUNDING.md](STATE_AND_ROUNDING.md) documents the complementary
full-memory recurrence, stability/decay, finite IEEE rounding, FTZ/DAZ, actual
287-tap FIR, and nonlinear-cell composition contracts. Their native checks cover
state copies, priming, arithmetic residuals, ring history, and actual Fuego jumps.

## Boundaries of the frequency-response theorem

The frequency-response theorems describe the fixed-coefficient, exact-real, steady-state frequency
response using the implementation's rounded coefficients. They do **not** prove:

- sample-by-sample IEEE arithmetic error bounds or overflow behavior;
- pole convergence, startup/priming transients or stopped-filter history;
- suppression throughout the transition region just above 24 kHz;
- the root interpolator's imaging, the output FIR, oscillator generation,
  nonlinear Fuego/audio DSP, or alias rejection of the complete synthesizer;
- arbitrary-signal transient/energy bounds or audio-thread deadlines.

Native tests continue to cover those modeled implementation paths where
applicable. No claim that the whole instrument is alias-free follows.
