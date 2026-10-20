## Why

The Controllers page now names the device each row is — the preset's device
name when the row's wizard id resolves, otherwise the bound MIDI input — and
lets a row's connect-time SysEx messages be read and edited. That work shipped
alongside a row-level pressure-mapping editor whose edits revert when the
section is reopened, and it left a browser test asserting a device label the
row no longer renders.

This change keeps the device label, the single Custom entry and connect-message
editing, and finishes what shipped incomplete: it removes the pressure-mapping
row editor rather than repairing it, covers the one connect-message path
nothing exercises, and corrects the browser assertions this work falsified.

## What Changes

- **Remove the pressure-mapping row editor.** The row's "Pressure mappings"
  list, its Add/Delete/field-commit handlers and node ids, the
  `MidiConfigViewModel` entry points behind them, the
  `MirrorPressureMappingChangeIntoOpenSession` mirroring it needed, the two
  token helpers whose only call sites live inside that block, and its ten
  tests all go. A pressure mapping attached to a grid cell is edited by opening
  that grid row, as it was before this work.

  The removal is not a new rule. The promoted grid requirements already say the
  page exposes "no toggle, note number, MIDI status, aftertouch, or
  pressure-mapping row" and that the rendered tree contains "no standalone
  aftertouch or polyphonic-pressure line item". The row editor contradicted
  promoted text; deleting it restores conformance, which is why this change's
  spec delta adds no clause about it.

- **Cover committing to a connect message other than the first.** Deletion by
  index is already covered by a shipped test that deletes index 1 on a
  two-message row. Committing by index is not covered by anything. The handler
  path is correct: on a two-message row, committing to index 1 leaves index 0
  at `F0 01 F7` unchanged and sets index 1 to `F0 55 F7`. The new test is a
  regression guard over working behaviour, not a fix.

- **Tighten the connect-message commit handler's arity guard.** This is real
  user-facing behaviour: `HandleConnectMessageCommit`'s guard on the number of
  `:`-separated parts moved from `< 3` to `< 2`. A value of
  `"<controllerIx>:<messageIx>:"` — committing an entirely empty hex field —
  tokenises to 2 parts, since `Split`'s `std::getline` loop drops the trailing
  empty token. The old `< 3` floor treated that as too few parts and returned
  before calling `SetConnectMessage` or `SetStatus`: committing an empty field
  did nothing and said nothing, a silent no-op indistinguishable from success.
  The new `< 2` floor lets that value reach `SetConnectMessage`, whose own
  emptiness check refuses it with a visible `Refused: ...` status, matching
  every other malformed edit. Covered by
  `TestConnectMessageEditCommitsValidAndRefusesInvalidUnchanged`'s empty-commit
  assertions.

- **Fix the browser spec's device assertions.** The manually-added-record test
  asserts the old kind-identity string and drives an add flow through controls
  the page no longer has. Its row adds through Custom and binds no endpoints,
  so it renders neither a preset device name nor a stored endpoint label — the
  corrected assertion names what that row actually renders. The suite fails
  nine of seventeen tests for a reason predating this work, where the old add
  controls were removed without every reference to them being enumerated.
  This change fixes the assertions this work falsified and reports the rest
  as still failing.

- **Correct the comments this work makes false**, inside the directories this
  change touches: the claim that connect messages and pressure mappings are
  unconditional on every kind, which the removal makes false; the
  `kControllerDeviceWidth` comment, which asserts the constant clears every
  listed device name while the check behind it measures only the library's own
  names; the construction-path comments claiming a cache or registry nobody
  configures behaves exactly as it did before, which stopped being true when the
  fallback registry grew from one device to three; and
  `ParseFiniteNumericToken`'s comment, which said "every caller" shares its
  wording -- true while the removed pressure-field handler was a second
  caller, false now that the grid mapping-field commit is the only one left;
  and two of the status-constant comments this change's own consolidation
  introduced call sites for: `kInvalidControllerIdentityStatus`'s comment
  named wizard-session identity mismatches, when all four of its call sites
  refuse an unparseable action token (one of the four also a missing
  snapshot-callback case), and `kControllerRecordChangedStatus`'s comment
  described only its wizard-revalidation call sites, when one of its three
  is a rename/delete action's identity lookup with no wizard involved.

## Capabilities

### New Capabilities
None.

### Modified Capabilities
- `synth-runtime-ui`: adds requirement text for the row device label, the add
  row's single Custom entry, and connect-time message editing, none of which
  any promoted requirement described — verified by reading the promoted spec,
  not assumed. The requirement's number is assigned when this change archives,
  against the highest id then present in
  `openspec/specs/synth-runtime-ui/spec.md`: several unarchived changes claim
  overlapping ids, and whichever archives first takes them.

## Impact

- `projects/synth/include/synth/ControllersPageUI.hpp`,
  `projects/synth/include/synth/MidiConfigViewModel.hpp`,
  `projects/synth/src/MidiConfigViewModel.cpp`: remove the pressure-mapping
  node ids, actions, handlers, view-model entry points, token helpers and
  open-session mirroring; correct the comments named above.
  `ControllersPageUI.hpp` also: introduce `kHostRejectedCommitStatus` for the
  "Refused: host rejected the instrument commit" text five call sites already
  shared, and use it at the three connect-message handlers
  (`HandleConnectMessageCommit`, `HandleConnectMessageDelete`,
  `HandleConnectMessageAdd`) that previously called `Commit()` without
  checking its return -- a host rejection there silently reported success,
  which this change closes the same way the wizard/add paths already handle
  it. The same sweep found six sibling status-literal families
  (`Refused: invalid controller identity`,
  `Refused: generated controller record is invalid`,
  `Refused: controller record changed; refresh and try again`,
  `Refused: current controller state is unavailable`, the runtime-config
  save-failure text, and the wizard's controller-profile-generation-failure
  text, the last shared by two byte-identical `SetWizardStatus` expressions),
  each duplicated across 2-4 call sites the same way; this change
  consolidates all six into named constants beside `kHostRejectedCommitStatus`.
  Separately, `ConnectMessageRow` and the new
  `ConnectMessageAddRow` switch their cross-axis layout from
  `rowLayout(..., scrollWidth, ...)` to `layout(..., Weight(1.0f), ...)`:
  both rows are nested inside the connect-messages `scroll.Column`, whose own
  resolved cross extent can be narrower than `scrollWidth`'s horizontal-
  scroll floor, so the fixed `scrollWidth` could overflow the column's
  actual bounds. `ConnectMessageAddRow` (new) wraps the existing Add button
  in its own row so the button's width/height style land on that row's own
  axes instead of the enclosing column's. A further stale-name sweep, the
  same class as `viewmodel_tests.cpp`'s below: seven comments across
  `MidiConfigViewModel.hpp`, `MidiConfigViewModel.cpp`, and
  `viewmodel_tests.cpp` (three of the seven) say `ControllersPage.hpp` where
  the file has been `ControllersPageUI.hpp` since before this change opened;
  this change corrects all seven to the real filename.
- `projects/synth/include/synth/ControllerWizardDiscoveryCache.hpp`: correct
  the same class of stale construction-path comment.
- `projects/synth/tests/controllers_page_ui_tests.cpp`: remove the ten
  pressure-mapping tests and their call sites; add the connect-message
  index-one test, sharing a new `SetUpTwoConnectMessageRow` setup helper with
  the existing index-one delete test; add a portable-tree assertion that no
  text containing "pressure mapping" is visible, the one absence check that
  still fails if a per-row pressure-mapping list is ever rendered again
  (neither of the existing aftertouch/polyphonic-pressure greps would catch
  it); add a bounds assertion on the connect-message Add button, covering
  the row's `Weight(1.0f)` cross-extent fix below; and add a test exercising
  the three connect-message handlers' host-rejection branches, using the
  harness's existing `commitSucceeds` knob to force `Commit()` to fail and
  asserting each handler's own state and status text rather than the
  wizard/add paths' shared coverage.
- `projects/synth/tests/viewmodel_tests.cpp`: a comment corrected by the same
  sweep.
- `projects/synth/juce/ControllersPageScreenshotHarness.cpp` (new, tracked as
  part of this change): an operator-review harness that renders the
  Controllers page through the real `ControllersPageSurface` /
  `MidiConfigViewModel` and the app's own MIDI catalog. Every state's
  underlying edit is produced by dispatching the page's real `ui::Action`
  values, except state 3 (the Preset combo's popup), which needs real JUCE
  widget calls (`showPopup()`, `getItemText()`, `dismissAllActiveMenus()`)
  because the popup is its own top-level component outside the page's node
  tree. Writes each rendered state to a PNG under an output directory that
  defaults to a `screenshots` folder beside the built binary (overridable by
  argv[1]) -- the six states named in this change's Delivery Gate, plus this
  harness's own positive-control and extra-check images, which are
  verification evidence that the harness itself can show a broken state, not
  part of what the operator reviews.
- `projects/synth/apps/miniapp/Makefile`: add the
  `controllers_page_screenshot_harness` build target (and its
  `build/controllers_page_screenshot_harness` alias), linking the new harness
  against the app's own `FroggersMidiCatalog.hpp` via the overridable
  `FROGGERS_APP_DIR` knob. Not part of `test`; invoked by name.
- `projects/synth/scripts/check_ui_boundary.sh`: widen
  `BACKEND_EXCLUDED_FROM_ALL` to also exclude
  `ControllersPageScreenshotHarness.cpp` from every backend layering scan.
  This is load-bearing, not cosmetic: the harness sits in `projects/synth/juce`
  (a scanned backend root) and deliberately includes producer headers
  (`ControllersPageUI.hpp`, `ControllerWizard.hpp`) to drive them directly --
  exactly what check 2 (a backend includes no component-library or
  producer header) exists to forbid for a real backend renderer, and what its
  own scan pattern names. Without this exclusion, adding the harness makes
  `check_ui_boundary.sh` fail the moment it scans this file.
- `projects/synth/browser/tests/fake-app.e2e.spec.ts`: fix the stale `.device`
  assertion and the add flow the manually-added-record test drives. Making that
  test's downstream assertions match what the page renders also required
  replacing its row-scoped rename assertion with a disclosure click and a
  page-scoped one, because the rename control is emitted only inside an
  expanded row. That is a test-only edit, reaching no production code, and it
  is named here rather than carried silently: the same locator breakage remains
  in a neighbouring rename/delete test, which this change leaves failing along
  with the rest of the inherited suite.
- Overlaps the pending `rework-controllers-block-editing`: its open-section
  edit-session model covers the same `SectionPresentation` and flush territory
  this change's removal touches. Whichever lands its `SectionPresentation` edit
  second re-reads the other's diff before touching `hiddenPressureMappings` or
  the flush path.
- Contradicts the pending `app-midi-catalog`'s spec delta, which still
  specifies the row's kind label and a Custom entry per device kind. The
  device label and single Custom entry already supersede that text in shipped
  code; reconciling that delta belongs to whoever archives it, and this change
  does not edit it.
- frogg3rs carries the paired change that corrects its own falsified promoted
  spec and bumps its pin.

## Delivery Gate

Before this merges to main, the operator reviews screenshots of the
Controllers page in these states: (1) an active row whose wizard id resolves,
showing the preset's device name on line one; (2) an active row with no
resolved wizard id, showing its bound MIDI input as the device label; (3) the
add row's Preset dropdown open, listing the app's own devices, one library
device per uncovered kind, and one Custom entry; (4) a row's expanded editor
showing its Connect messages list with an existing message, an Add button, and
a delete button; (5) that same editor after a malformed connect-message edit,
showing the refusal status text; (6) a row's expanded editor showing no
"Pressure mappings" list, with a working grid mapping row present in the same
image.

State 6 shows the absence only. That a pressure mapping attached to a grid cell
survives is not visible in any image: the page represents such a mapping as the
grid button itself, so a grid row holding one is pixel-identical to one that
does not. That half is carried by the grid tests, which pass, and by the
promoted grid requirements the removal restores conformance with — not by the
operator's eye.

## Cut, with the evidence that cut it

- **Widening the device-label width check.** Measured with the same glyph
  technique and allowance the existing cases use: the longest real device name
  renders at 141.1px and the bound-device fallback label at 221.6px, against an
  allowance of 308px, with the instrument proven live first (one character at
  6.9px against 552.8px for eighty). There is no defect to catch. The proposed
  literal would also have been a third unsynchronised copy of a name another
  repository owns, and the widening step it carried was impossible: the two
  header-width `static_assert`s it promised to preserve already hold with zero
  slack, so any widening breaks them.
- **A test for a connect-message index that does not exist.** Indices reach the
  handler from the rendered row, so a player cannot produce one out of range.
  The refusal a player can reach — text that is not a complete SysEx message —
  is already covered.
- **A comment recording which commit broke the browser suite.** A comment says
  what the code does, not what happened to it.
