# Design — browser-slider-value-readout

Anchors verified 2026-08-19 at Sheaf `fix-out-of-tree-app-gaps` working
tree (c0bf7b48 + uncommitted ui.ts drag-scale work).

## Data flow

`Builder::Slider` (app side) → node record {value, minValue, maxValue,
step, label} on the wire → browser `BrowserUiBackend.createElement`
(`ui.ts:133-141`; Slider branch `:136` creates `<input type="range">`)
→ `updateControl` (`ui.ts:266-276`; Slider branch sets min/max/step,
sets `input.value` only when the input is not focused, sets disabled).
The JUCE backend's readout for the same node:
`PortableJuceBackend.hpp:1160-1178` (TextBoxBelow, 56x18).

## Change

- `createElement` Slider branch: additionally append a readout element
  (`<output>`; one element, no wrapper) after the `<input>`.
- `updateControl` Slider branch: set the readout's text from
  `node.value`, formatted by the PINNED algorithm below (preflight
  defect 2 fix — prose is not a spec):
  - Decimal count N, from the node's `step`, mirroring JUCE's own
    default exactly (`juce_Slider.cpp:144-162` `updateRange()`):
    `N = 7; v = round(|step| * 1e7); while (v % 10 === 0 && N > 0) { N--; v = floor(v / 10); }`
    (fixed-point trailing-zero count over the step — NOT "count the
    typed digits").
  - `step === 0` (continuous): JUCE's literal default leaves N = 7;
    pin N = 7 for parity. No current producer passes step 0
    (braid-4/miniapp all pass 0.001f; audited) but the rule must be
    total.
  - Rendering is FIXED-WIDTH at N decimals for every value (JUCE
    `getTextFromValue` → `String(val, N)`, `juce_Slider.cpp:1647-1660`)
    — never per-value trimming; step 0.01 shows "5.00", not "5".
  - Rounding: `value.toFixed(N)` for N > 0 and `String(Math.round(value))`
    for N = 0, WITH a required byte-parity test table including rounding
    boundaries (at least: value 2.675 step 0.01; value 0.5 step 1;
    a negative value; min and max endpoints). If any table row's
    `toFixed`/`Math.round` output diverges from JUCE's iostream-fixed
    formatting (juce_String.cpp:476-503, std::ostream + ios_base::fixed,
    classic locale) for the same binary double, the JS side is adjusted to
    match JUCE and the divergent case is recorded in the test as the
    authority. Both backends format the SAME binary double, so
    divergence is confined to decimal-tie handling — the table makes it
    observable instead of latent.
  Live-drag updates already flow through `updateControl` on every
  frame; the readout needs its own update also on the `input` event so
  it tracks a drag BETWEEN frames (the input listener at `:136` is the
  seam — update the sibling readout there before dispatching), and both
  update sites call the ONE shared formatter (§8).
- Layout/metrics (REVISED 2026-08-19 after operator smoke + postflight;
  the original overlay strategy is abandoned, recorded here rather than
  deleted): the node's wire-set bounds do NOT grow — the surface owns
  geometry — and the readout is carved OUT of them as its own strip at
  the bottom, with the range input shortened above it, mirroring how
  JUCE's `TextBoxBelow` splits its own bounds. The first cut instead
  OVERLAID the readout on the full-height track: the operator's smoke
  showed digits sitting across the filled track and vanishing under the
  thumb, unreadable at most values.
  The strip adapts to the node (`sliderReadoutStrip`, ui.ts): at most
  `SLIDER_READOUT_HEIGHT_PX` (14 — this backend's 9px glyph plus
  breathing room, against JUCE's 18px at its own default text size),
  at most `SLIDER_READOUT_MAX_SHARE` (0.4) of the node's height so the
  track always keeps the majority, and dropped entirely below
  `SLIDER_READOUT_MIN_PX` (8) where it could neither letter a glyph nor
  fit. A fixed height would overflow the bounds and clamp the track's
  hit area to zero on any slider ≤14px tall — postflight finding, now
  pinned by two boundary tests.
  `pointer-events: none` remains on the readout regardless, so it can
  never become a hit target even if a future layout overlaps it.
- Styling: text colour follows the node's carried text style per sru-45
  where present, else the backend default; no hardcoded per-app colours.

## Known limitation (postflight-recorded 2026-08-19)

At N = 0 the implementation uses `String(value)` — chosen because the
design's pinned `String(Math.round(value))` was empirically shown to
diverge from real JUCE output on the required 0.5/step-1 parity row
(the contingency clause fired, JUCE recorded as authority in the test).
`String(value)` matches JUCE across every parity row AND across the
value ranges any current producer emits, but it diverges from
iostream's default significant-digit behavior for very large
magnitudes (roughly ≥1e6) at N = 0. No producer emits such a slider
today (BPM ≤ 300; scene blend 0-2; braid-4/miniapp use 0.001 steps), so
sru-59's cross-backend text guarantee holds in practice — but a future
large-magnitude, integer-step slider would violate it silently. Pinned
here rather than left implicit; carried as a follow-up task below.

## Constraints

- Zero app-side edits; zero JUCE-side edits.
- The existing dirty drag-scale work in `ui.ts` (uncommitted, reviewed
  in the frogg3rs session) must remain byte-intact around this change;
  this change layers on top and both land on the same branch as
  SEPARATE commits/PRs per the operator.
- All 65 existing ui-backend tests stay green unmodified.

## Testing

- ui-backend.spec.ts: readout exists for a Slider node; shows the
  formatted initial value; follows a wire value update; follows a
  simulated drag `input` event between frames; integer-vs-fractional
  step formatting; disabled slider still shows its value.
- Pointer routing (REVISED with the layout above): a press on the TRACK
  resolves to the input, the readout is never itself the hit target,
  and the readout starts at or below the track's bottom edge. (The
  original "hit-test the input THROUGH the readout" assertion only made
  sense while the readout overlaid the track.)
- Strip boundaries (postflight-added): a short node (12px) drops the
  strip entirely and keeps a full-height, still-draggable track; a
  mid-height node (24px) scales the strip down rather than overflowing,
  and the track keeps the majority of the box.

## Risks

- Focused-input guard (`document.activeElement !== input`) means wire
  updates skip the input while dragging — the readout must NOT be
  gated by that guard or it will freeze mid-drag (it has no focus
  semantics of its own).
