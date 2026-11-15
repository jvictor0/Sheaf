# Tasks — browser-slider-value-readout

Gate: `cd projects/synth/browser && npm test` (ui-backend suite) green —
all pre-existing tests unmodified and green, new tests green. No commits
until the operator-gated commit/PR step.

## 1. Readout implementation + tests

- [ ] 1.1 `ui.ts`: readout element in the Slider create path; updates in
      the Slider update path AND the input-event seam (design: the
      focused-input guard must not gate the readout). Step-derived
      formatting shared by both update sites (single formatter, §8).
- [ ] 1.2 `ui-backend.spec.ts`: the seven test cases the design lists,
      PLUS the pinned-format byte-parity table (rounding boundaries:
      2.675/step 0.01, 0.5/step 1, a negative value, range endpoints)
      and the step===0 → 7-decimals case (preflight defect-2 fix).
- [ ] 1.3 Full ui-backend suite green; report counts (existing N green
      unmodified + new M green). Rebuild browser dist (`npm run build`)
      and verify the frogg3rs local site shows live values on BPM and
      Scene blend (evidence: repackage + screenshot or DOM text read).

## 2. Postflight audit (separate dispatch), then operator-gated PR

- [ ] 2.1 §14 postflight: implementation vs this change's artifacts;
      §8 re-run against the diff (the formatter is a new named concept —
      enumerate what it makes redundant).
- [x] 2.2 Apply postflight findings — postflight returned CLEAN with two
      minors, both addressed as records rather than code: the
      large-magnitude N=0 divergence is now pinned in design.md's Known
      limitation section (auditor's own recommendation: track, don't
      block) and must be disclosed in the PR body; the stale
      `package-contract.test.mjs` line citation in the implementer
      report (`:254` vs actual `:292`) does not affect the substantive
      pre-existing-failure claim, which the audit verified against git
      history.
- [ ] 2.2b FOLLOW-UP (not this PR): a large-magnitude parity row for
      N=0 sliders, either by matching iostream's significant-digit
      behavior or by constraining producers; open when a producer needs
      it, cite this change's Known limitation.
- [ ] 2.3 OPERATOR GATE: separate commit + PR on the Sheaf repo for THIS
      change only.
