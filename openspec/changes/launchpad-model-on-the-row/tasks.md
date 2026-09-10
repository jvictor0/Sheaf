# Tasks — `launchpad-model-on-the-row`

## 1. Count what a recorded model touches

- [x] 1.1 Enumerate, by operand, every site that builds a launchpad profile or
      a launchpad association outside the page: grep `launchpadPosition`,
      `LaunchpadController::`, and `LaunchpadGridPosition` across
      `projects/synth/` including tests. Report FOUND per file and which need
      the profile's model set for the profile to stay valid. This count is the
      change's own blast radius and is written down before any edit.
      Done: 63 sites FOUND (19 production, 44 tests). No production path builds
      a profile whose associations disagree with each other; the three mixed
      constructions in the tree are all fixtures of other types or of an
      unvalidated round trip. 55 sites would have to declare the model under a
      validity rule, which is why this change does not add one -- see the
      proposal. The writers that set the field are 3.

## 2. The recorded model

- [ ] 2.1 Add `launchpadModel` to `MidiControllerProfileConfig`.
- [x] 2.2 Carry the field through the profile's `ToJSON`/`FromJSON`, deriving
      it on read when absent from the first positioned association. Test all
      three arms: field present, field absent with positions, field absent with
      none.
- [x] 2.3 Set the field at the three writers that choose a model
      (`LaunchpadDefaultProfileConfig`, the view model's Launchpad branch of
      `AddController`, and the application's presets), and report FOUND vs
      CHANGED against task 1.1's count.

## 3. The edit API

- [x] 3.1 `MidiConfigViewModel::LaunchpadModel(controllerIx)`.
- [x] 3.2 `MidiConfigViewModel::SetLaunchpadModel(controllerIx, model, out,
      reason)`: rewrite every position, refuse when one lies outside the target
      shape naming it, commit through the same copy/validate/hand-back shape as
      `RestoreController`.
- [x] 3.3 Point `CurrentLaunchpadVariant`'s four call sites at the recorded
      model and delete the derivation.

## 4. The control

- [x] 4.1 Carry the row's model on `MidiControllerRowVM`.
- [x] 4.2 Emit the Variant combo on launchpad rows only, restoring
      `NodeIds::ControllerVariant` and `Actions::kVariantSelect`.
- [x] 4.3 Handle the action; render the refusal as the page's own status.
- [x] 4.4 Correct the comment at `juce/ControllersPageSimulationTests.cpp:115`,
      which listed the selector among the page's uncaptioned controls: this one
      carries a "Variant" caption. The simulation's own harness seeds a
      launchpad row (`juce/ControllersPageHarness.hpp:66`), so every simulated
      step renders the selector and holds it to the page's caption and layout
      rules; the control is driven end to end by
      `TestLaunchpadRowOffersVariantAndRetargetsItsPads` in
      `tests/controllers_page_ui_tests.cpp`.

## 5. Gates

- [x] 5.1 Run the library gate and every test binary by path; name each gate,
      when it last ran, and what moved under it. Done 2026-09-09:
      `make -C projects/synth test` stopped at `braid4_deadline_tests`, whose
      two 96 kHz cases fail on this machine before this change and measure DSP
      timing it does not touch (44.1 and 48 kHz pass in the same run). Every
      binary at and after that point was run by path: 20 binaries, all exit 0,
      and the whole log carries exactly those 2 failures. The JUCE page
      simulation (`apps/miniapp/build/controllers_page_simulation_tests`) was
      rebuilt from scratch and passed. `miniapp_system_tests` was rebuilt after
      the hygiene fix below and reports no warnings.

## 6. Delivery

- [ ] 6.1 Branch of the fork, next sequential pull request.
- [ ] 6.2 In `frogg3rs`: set the model on the three Launchpad presets, move the
      submodule pin, and correct
      `openspec/specs/froggers-sheaf-runtime-app/spec.md:294` and its scenario
      at `:313` — the Variant clause is true again, the row-preset clause goes.
