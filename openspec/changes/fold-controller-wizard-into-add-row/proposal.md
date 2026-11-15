## Why

Two player stories, for any MIDI device:

1. A player plugs in a controller that has a preset, opens Controllers, adds
   that preset, changes one of its mappings on the page, and plays. On the next
   visit the controller is set up the same way.
2. A player adds a device that has no preset, or builds a device's mapping from
   scratch, as Custom: picks its ports and enters every mapping by hand. On the
   next visit that mapping is still there.

Both were walked on the live frogg3rs site (Chrome, MIDI Fighter Twister
attached, Sheaf at `f73d4202`), and both fail or cost more than they should:

- **Edits are lost unless the player leaves by Back.** Adding the Twister
  preset, changing one System Messages entry and reloading left no row at all.
  Switching to Sync instead of pressing Back did the same. Only Back saved.
  A Custom row with a hand-entered mapping entry behaved the same way. In the
  code, `ControllersPageSurface::Commit` edits the running instrument and never
  saves. Saving happens only on lifecycle buttons, on wizard Submit/Ignore, and
  on Back (`RuntimePageBackSavesConfiguration`). A player who edits and then
  plays and closes the tab loses the whole setup.
- **A new row hides what it installed.** An added row and each of its sections
  start collapsed. The preset's mappings sit three clicks below the Add button,
  so "change one mapping" first means finding them.
- **The Configuration Wizard is a second, empty door to Add.** The
  Configuration Wizard button, the per-device Configure, and a released row's
  Configure all open a wizard page. For an application's preset that page has
  no fields; `scw-5` specifies the empty form. The player sees only
  Back/Cancel/Submit/Ignore, and Submit installs what Add installs. The add row
  already runs the same discovery to bind a connected device's ports
  (`HandleAddController` calls `DiscoverControllerWizards`). It also generates
  the profile through the same wizard (`InstallDescriptorProfile`). Mapping
  edits happen on the row in both stories. The wizard page serves neither.
- **Release has no way back without the wizard.** Ignore and Release make a
  record Blacklisted. Release keeps the profile "as dormant reconfiguration seed
  data" for the wizard form, and the wizard form is the only path back to
  Active. Reclaim, the released row's other button, deletes the record. Remove
  the wizard page and Release becomes a one-way trip whose only exit is
  deletion.
- **In an application whose patches carry mappings, a saved patch undoes the
  page on the next visit.** frogg3rs sets `patchCarriesMappings`. At launch,
  the engine loads the runtime configuration and then the newest saved patch,
  whose instrument replaces the configuration's. So every Controllers edit made
  after a patch was saved is gone on the next visit, however it was saved.
- **An APC40 mkII added as its Ableton preset never binds.** Discovery gives
  the port pair to the first preset that matches, which is Generic, and Add
  binds only a pair assigned to the chosen preset.
- **Revert duplicates Load and New.** With a patch open, the File page's Revert
  reloads that patch's newest save, which Load does when the player picks it.
  With none open, it runs New.

## What Changes

- **Every committed edit is saved.** Each Controllers-page action that commits
  to the instrument also saves the runtime configuration. That covers adding a
  controller, choosing a port, editing a mapping field, adding or deleting a
  mapping entry or block, editing connect messages, Rename, Delete and Restore.
  A failed save shows a status and leaves the edit in place. Back no longer
  saves on the Controllers page's behalf; Audio and Sync are unchanged.
- **A newly added row opens.** Adding a controller, from a preset or as Custom,
  shows its row expanded with every section open. The row's disclosure and
  section toggles collapse it as they do for any row.
- **Connected devices are status, and the add row starts on one.** "Available
  controllers" becomes read-only status, with no buttons. It lists each
  recognized connected device that no row uses, by its port names and the
  presets that match it. Every other connected port is listed as "Other inputs"
  or "Other outputs". When the player has not chosen a Preset, the add row
  starts on the first listed device's first preset. One press of Add then
  installs it, bound to that device.
- **The wizard's entry points go.** This removes the Configuration Wizard
  button, the wizard page (form and chooser), Configure and Ignore on the
  connected-device list, Release on active rows, and Configure on released
  rows. A released record loaded from an earlier configuration still shows its
  Released badge and stored ports, and offers Delete. One Delete now removes a
  record of either disposition.
- **Code only the removed UI reached goes.** That means the wizard session
  state and its handlers, the configuration forms' user-interface half (tree
  building, action dispatch, form layout), reconfigure seeding and its warning,
  and tests of removed behaviour. The registry, discovery, profile generation,
  Restore, and the Blacklisted disposition with its persistence and
  reconciliation all stay. Add and Restore run on the first three, and saved
  configurations can hold released records.
- **Add binds the chosen preset.** Add binds the first unused connected pair
  whose port names match the chosen preset's aliases. So an APC40 mkII binds as
  either of its presets, and "Available controllers" lists it once with both.
  Ports outside any waiting device are listed as "Other inputs" and "Other
  outputs".
- **A relaunch restores the setup the player last had.** The runtime
  configuration holds the controller setup across a relaunch. The patch loaded
  at launch restores its sound. Opening a patch from the File page applies its
  sound and setup and saves the configuration. Launch reopens the patch version
  last opened or saved, and falls back to the newest saved version when there
  is no record.
- **Revert goes.** Load and New already serve its stories on the File page.
- **Docs and specs follow.** The README's wizard section and
  `docs/coverage.md` are rewritten to match, and the deltas below update the
  requirements.

## Impact

- `projects/synth/include/synth/`: `ControllersPageUI.hpp`,
  `ControllerWizard.hpp`, `MidiConfigViewModel.hpp`, `RuntimePagePolicy.hpp`,
  `Engine.hpp`, `PatchPersistence.hpp`, `RuntimeFileService.hpp`,
  `RuntimePages.hpp`, `RuntimeMainComponent.hpp`, `MidiController.hpp` (a
  comment), `browser/BrowserRuntimeMainServices.hpp`, `browser/BrowserRuntime.hpp`,
  and the runtime configuration's persistence.
- `projects/synth/src/`: `ControllerWizard.cpp`, `MidiConfigViewModel.cpp`,
  `PatchPersistence.cpp`.
- `projects/synth/runtime/`: `JuceRuntimeMainServices.hpp`, `Runtime.hpp`.
- `projects/synth/scripts/check_ui_boundary.sh` (a comment).
- `projects/synth/tests/`: `controllers_page_ui_tests.cpp`,
  `browser_runtime_contract_tests.cpp`, `contract_tests.cpp`,
  `engine_tests.cpp`, `portable_ui_tests.cpp`,
  `runtime_main_component_tests.cpp`, `controller_wizard_tests.cpp`,
  `viewmodel_tests.cpp`, `rig_tests.cpp`, `parameter_modulation_tests.cpp`,
  `support/SynthRig.hpp`.
- `projects/synth/juce/`: `ControllersPageSimulationTests.cpp`,
  `RuntimeShellSessionTests.cpp`, `ControllersPageScreenshotHarness.cpp`,
  `PortableJuceBackendTests.cpp`, `RuntimePagesJuceTests.cpp`,
  `RuntimePagesJuce.hpp`, and any File page test naming Revert.
- `projects/synth/browser/tests/`: `fake-app.e2e.spec.ts`,
  `visual-criteria.spec.ts`.
- `projects/synth/README.md`, `projects/synth/docs/coverage.md`.
- `openspec/`: deltas to `synth-runtime-ui`, `synth-controller-wizards`,
  `synth-midi-instrument`, `synth-app-runtime`, `synth-portable-runtime-shell`
  and `synth-patch-persistence`. Some of them modify
  requirements that still-open changes add: sru-59, sru-60, sru-61 and sru-62 come from
  `app-midi-catalog`, and sru-4 cites sru-63, which
  `finish-controller-row-device-editing` adds. The removed scw-4 names the base
  spec's header. `app-midi-catalog` modifies the same requirement under a
  different title, so whichever change archives second reconciles the two.
- Every Sheaf application loses the wizard page and Revert, and on relaunch
  reopens the patch version last opened or saved. The library MIDI Fighter
  Twister wizard still generates its six-button default through Add, for
  applications without their own Twister preset, and that default is edited
  on the row.
- `frogg3rs-transport-and-shift-ux` edits the same Controllers-page files on
  the same base. Whichever Sheaf branch is pushed second rebases onto the
  other's tip.
- frogg3rs carries this change in its own
  `carry-fold-controller-wizard-into-add-row`: the pin, one test call, its
  spec, and its manual.
