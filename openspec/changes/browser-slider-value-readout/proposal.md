# Proposal — `browser-slider-value-readout`

**Created 2026-08-19 at the operator's instruction**, from a live-site
smoke finding on the frogg3rs browser host: the Scene-blend and BPM
sliders show no numeric value in the browser, while the identical
portable surface shows one on desktop.

## Why

(The two sliders the smoke test noticed are app-side nodes in the
consuming repo — FroggersTiga `app/FroggersUiSurface.hpp:1297-1299`
builds the BPM slider, scene-blend adjacent — but the defect and the
fix are generic over `NodeKind::Slider`; Sheaf's own braid-4/miniapp
sliders have the same gap.)

The two backends render the same `Slider` node differently: the JUCE
backend gives every slider a numeric readout
(`slider->setTextBoxStyle(juce::Slider::TextBoxBelow, false, 56, 18)`,
`projects/synth/juce/PortableJuceBackend.hpp:1162`), while the browser
backend creates a bare `<input type="range">` and nothing else
(`projects/synth/browser/src/ui.ts:136`) — there is no element a value
could appear in. This breaks the one-surface-every-host contract at the
backend layer (`synth-runtime-ui` sru-45's own standard: "both backends
agree"). An app-side workaround (per-host value labels) would fork the
surface per host and duplicate what the JUCE backend already provides —
rejected under omni §8.

## What Changes

- `synth-runtime-ui` (delta, ADDED requirement): a `Slider` node SHALL
  present its current numeric value in every backend, updating live,
  formatted equivalently across backends.
- Code: browser backend only — `browser/src/ui.ts` slider create/update
  paths gain a value readout element; unit coverage in
  `browser/tests/ui-backend.spec.ts`.

## Impact

- Affected specs: `synth-runtime-ui` (ADDED requirement).
- Affected code: `projects/synth/browser/src/ui.ts` (+ its spec file).
  No JUCE-side change (it already conforms). No app-side change.
- Consumers: every browser-hosted app gains the readout; the frogg3rs
  site regains desktop parity with zero app-side work — exactly the
  portability argument the surface design makes.
- Delivery: own branch + PR (operator instruction: separate PR per fix,
  landed only after preflight audit, implementation, and postflight
  audit with fixes applied).
