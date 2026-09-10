# Proposal — `launchpad-model-on-the-row`

**Created 2026-09-09.** Paths are relative to `projects/synth/` unless the
line says otherwise. Line numbers are 2026-09-09 reads of the fork's
`shift-and-file-export` at `ddb14693`, the commit `frogg3rs` currently pins.

## Why

A Launchpad controller's model cannot be chosen. A row created from the
`Custom (Launchpad)` preset has no mappings, so it has nothing to read a model
from and behaves as a Launchpad X; a row created from a model preset carries
that model in its pads and cannot be pointed at another unit.

The page used to offer this. `185a7a09` ("Give a controller row one control per
job") removed the Variant combo along with the row's preset combo, and said
why:

> The Launchpad Variant menu beside it had no state behind it: it derived the
> model from the first mapping carrying a grid position and rewrote existing
> mappings rather than recording a choice, so on a row with no mappings it
> wrote nothing and read back Launchpad X.

That diagnosis stands, and it is the design this change follows: the model
becomes recorded state rather than something read back out of the mappings.
What the library still does today is the derivation the removal was aimed at —
`CurrentLaunchpadVariant` (`src/MidiConfigViewModel.cpp:3349`) returns the
first launchpad association's controller, or Launchpad X when there is none,
and four call sites seed new rows and blocks from it (`:3518`, `:3573`,
`:3725`, `:3779`).

Three documents still describe the control that was removed:

- `openspec/specs/synth-runtime-ui/spec.md:396` lists "changes launchpad
  variant controls" among the workflows sru-15 preserves.
- `juce/ControllersPageSimulationTests.cpp:115` names "the variant selector"
  among the page's uncaptioned controls.
- In the application repository, `openspec/specs/froggers-sheaf-runtime-app/spec.md:294`
  requires the controller header's identity line to hold "name, device kind,
  Preset, and Variant for a Launchpad", and its scenario at `:313` requires a
  Twister row to show "the Preset selector on the first line".

Rendering the page's own node tree for an expanded Launchpad row today yields
a disclosure button, the two endpoint combos, Delete, Release, the rename
field and button, the section toggle, and the mapping cells. Neither selector
is there. The Variant half of those claims is restored by this change; the
row-preset half was removed deliberately and its text goes.

## What Changes

- `include/synth/MidiController.hpp`: `MidiControllerProfileConfig` gains
  `launchpadModel` (default `LaunchpadController::LaunchpadX`), meaningful
  only for the launchpad kind. The profile is the right home rather than the
  slot: it is what a wizard generates, what "matches its preset" compares,
  what a Blacklisted record keeps dormant, and what persists under `profile`.
- `src/MidiController.cpp`: `ToJSON`/`FromJSON` for the profile (`:2716`,
  `:2751`) carry the field, and a profile parsed without it takes the model of
  its first positioned association, else Launchpad X — which is what today's
  stored records mean.

  `ProfileConfigValidForKind` (`:3442`) is deliberately NOT given a rule that
  an association must name the profile's own model. The enumeration this
  change opens with counted 63 sites that construct a launchpad association or
  profile; 55 of them would have to start declaring the model to satisfy such
  a rule, and one test that round-trips a deliberately mixed profile
  (`tests/parameter_modulation_tests.cpp:13401`) would become invalid by
  construction. The runtime itself has never required agreement:
  `CreateMidiControllerProfileImpl` (`:3003`) validates nothing and routes each
  association through `launchpadOutputFor` on that association's own model
  (`:3083`). The two stay in step by construction instead — every writer already
  threads one model through a whole profile (`LaunchpadDefaultProfileConfig`
  drops a reset position whose model disagrees, `:3362`; the block paths stamp
  each cell from the block's own field, `src/MidiConfigBlocks.cpp:470`,
  `:558`), and this change points the seeds at the recorded model.
- `src/MidiConfigViewModel.cpp`: `CurrentLaunchpadVariant` and its four call
  sites read the profile's model instead of the first association.
  `LaunchpadModel(controllerIx)` and `SetLaunchpadModel(controllerIx, model,
  out, reason)` join the existing edit API, shaped like `RestoreController`
  (`:3032`) and `SetEndpointRef` (`:3069`): copy, rewrite, validate, hand back.
  A model change that would put an existing pad outside the target's shape is
  refused, naming the pad — Launchpad X and Mini MK3 share a shape, and the
  Pro MK3's extra column and rows have nowhere to land on either.
- `include/synth/ControllersPageUI.hpp`: the controller row's identity line
  gains a "Variant" combo for launchpad rows, restoring the node id
  `NodeIds::ControllerVariant` and the action `Actions::kVariantSelect` the
  removed control used, so stored ids and dispatch names stay as they were.
- `include/synth/MidiConfigViewModel.hpp`: `MidiControllerRowVM` carries the
  row's model for the page to display.
- `juce/ControllersPageSimulationTests.cpp:115`: the comment names the
  selector as present again, and the simulation drives it.

## Impact

- Affected specs: `synth-midi-instrument` (ADDED: the profile records the
  model), `synth-runtime-ui` (ADDED: the row offers the control). sru-15's
  workflow list, which still names "changes launchpad variant controls"
  (`openspec/specs/synth-runtime-ui/spec.md:396`), becomes true again and needs
  no edit.
- Affected code: as above. Without the validity rule, the writers that must
  set the field are the ones that already choose a model: the library's
  `LaunchpadDefaultProfileConfig` (`src/MidiController.cpp:3333`), the view
  model's own Launchpad branch of `AddController`
  (`src/MidiConfigViewModel.cpp:2893`), and the application's three Launchpad
  presets. Existing tests that build a profile through the factory inherit it;
  the few that push associations into a config by hand keep working, since
  nothing rejects them.
- Persistence: `kMidiInstrumentSchemaVersion`
  (`include/synth/MidiController.hpp:1120`) stays at 2. The new field is
  optional on read and derived when absent, so a stored instrument keeps the
  model its pads already imply, and an older build reading a newer file
  ignores a field it does not know.
- Application half (`frogg3rs`): the three Launchpad presets set the model on
  their config; `openspec/specs/froggers-sheaf-runtime-app` is corrected in
  the same change — the identity line's Variant clause becomes true again and
  the row-preset clause is struck, including the scenario at `:313`.
- Hygiene (step zero, over the tree this change touches): the only finding in
  `projects/synth/tests` was a `-Wrange-loop-construct` warning at
  `tests/miniapp_system_tests.cpp:530`, where a structured binding copied a
  `std::pair<Color, Color>` per iteration. Fixed here, in this change, as a
  one-word change to a reference binding; the file rebuilds warning-free.
- Delivery: the Sheaf half on a branch of the fork, delivered as the next
  sequential pull request; then the pin move and the application half pushed
  to `frogg3rs` `main`.
