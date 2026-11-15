## Context

The Controllers page offers two ways to set up a controller. The add row picks
a Preset (a registry descriptor, or Custom) and installs a row. The
Configuration Wizard picks a connected, recognized device and opens a form
whose Submit installs a row. Upstream built them in that order: the manual add
row on 2026-07-02, then the wizard on 2026-07-25 as "a three-click path for
known hardware", and the wizard's design lists "replacing the ... manual add
flow" as a non-goal. `app-midi-catalog` (#13) then routed the add row's named
presets through the wizard registry and `GenerateProfile`, so both doors now
reach the same machinery.

This design serves the two stories in `proposal.md`: tweak any preset on the
page, and build any device from scratch as Custom. Both must survive a reload.

## Data flow

A player's action on the page is dispatched as a `ui::Action` to
`ControllersPageSurface::DispatchAction` → `HandleAction`. The handler builds a
new `MidiInstrumentConfig` from the view model's snapshot, and
`ControllersPageSurface::Commit` passes it to `m_callbacks.commitInstrument`,
which each host wires to `engine.EditInstrument`
(`BrowserRuntimeMainServices.hpp`, `JuceRuntimeMainServices.hpp`).
Reconciliation then opens or closes endpoints. Saving goes through the
separate `m_callbacks.saveRuntimeConfiguration` → `Engine::SaveRuntimeConfiguration`
→ `SaveRuntimeConfigFile`, which writes the whole runtime configuration as one
JSON file by atomic rename. In the browser, the worker then sees the
persistence-dirty flag and `persistence.scheduleSync()` flushes the virtual
file system to IndexedDB after a 100 ms debounce.

Today `Commit` stops after `commitInstrument`. Four call paths add
`SaveCommittedWizardAction` after committing: new-candidate Submit and Ignore,
lifecycle actions (Rename, Delete, Release, Reclaim, Restore), and
existing-record Submit. Eleven commit paths never save: port selection,
Variant, mapping field, mapping entry add and delete, the three connect-message
actions, and both branches of Add. Those edits reach the engine and persist
only when `ReturnToApplication` saves on Back.

## Decisions

### D1. `Commit` saves

`Commit` calls `saveRuntimeConfiguration` after a successful
`commitInstrument`. That makes one definition site for "an edit on this page is
persisted", and every handler inherits it. The separate
`SaveCommittedWizardAction` calls go. A failed save does not undo the commit,
which matches what the lifecycle paths do today.

The page status after a commit is also set in one place. Today each handler
calls `SetStatus` with its own success text right after `Commit`. That text
would overwrite a save-failure status, so the player would see "OK" and lose
the edit on the next visit. `Commit` therefore takes the handler's success
text and sets exactly one status:
- `kHostRejectedCommitStatus` when the host refuses the instrument;
- `kRuntimeConfigSaveFailedStatus` when the commit lands and the save fails;
- the handler's text otherwise.

Handlers stop setting a status after `Commit`. `Commit`'s return value still
means "the instrument was committed", so handlers that return early on a
refusal keep doing so.

With every commit saving, the Controllers page's save on Back is redundant.
`RuntimePageBackSavesConfiguration` stops returning true for Controllers;
Audio and Sync keep saving on Back, since their edits do not go through this
`Commit`.

In the browser, mapping text fields dispatch on every keystroke (`ui.ts` binds
the field's `input` event), so a mapping field saves on every keystroke that
leaves it parseable. Each save serializes the runtime configuration and renames
one file, and the browser's IndexedDB write is already debounced. The desktop
host's text field commits on Return and on losing focus
(`PortableJuceBackend.hpp`), so it saves once per finished entry. The design
takes that cost rather than adding a second debounce.

### D2. A newly added row opens with every section open

Expand state lives in `MidiConfigViewModel::expandState_`, keyed by controller
name, and `Rebuild` gives a name it has not seen a collapsed entry. After
`HandleAddController` commits, the page sets that name's state to expanded,
with every section it lists open. It uses the view model's existing toggle
state, so the row's disclosure and section toggles collapse it as for any row.
Rows that appear any other way, such as loading a configuration, still start
collapsed. The view model's rule that a re-used name does not inherit a
deleted record's state stays as it is. The page then opens the new row on top
of it.

### D3. Connected devices are status; the add row starts on one

`m_discovery` is refreshed on every UI tick (`RuntimeMainComponent::Refresh` →
`RefreshControllers`), so it is current whenever the page builds. The
"Available controllers" block becomes read-only status. Each waiting device is one connected input and output
pair that no row uses and whose port names match a preset's aliases. It is
listed once, by its port names followed by the name of every preset that
matches it. An APC40 mkII therefore reads as one device with both of its
presets, "Akai APC40 mkII (Generic)" and "Akai APC40 mkII (Ableton)". When no
device is waiting, the block shows "No connected controller is waiting to be set
up". Connected ports outside any waiting device are listed as "Other inputs: …"
and "Other outputs: …". Those include:
- ports no preset's aliases match;
- a recognized device whose other port is missing;
- devices whose preset has no aliases (the library Launchpad and WRLD.Bldr
  descriptors name a profile kind, not one physical device).

"Other" is true of all three. A label saying a port has no preset would be
false for the last two.

`EffectiveAddPresetId` returns the player's chosen draft when there is one.
Otherwise it returns the first matching preset of the first waiting device, or
the registry's first descriptor when no device is waiting.

Discovery gives a port pair to the first descriptor whose aliases match, so
the APC40 pair belongs to Generic. `HandleAddController` binds only a candidate
carrying the chosen preset's wizard id, so adding Ableton with an APC40
connected leaves both ports at "(none)". Add must instead bind the first unused
connected pair whose port names match the chosen preset's own aliases. Then
Add binds any preset that matches the device, which is what sru-4 already
promises ("bound to a matching connected pair … when one is present").

A System Messages row's kind combo offers the application catalog's kinds
(sru-59). The library WRLD.Bldr default stores Hold Reset, Hold Random, Hold
Random Mod and Hold Gesture Select entries. frogg3rs's catalog lacks those
kinds, so the combo finds no match and shows its first option, "Param Inc/Dec",
which is a job the row does not have. The combo therefore also lists the row's
own stored kind, by its name, when the catalog lacks it. It shows that kind as
the current value, and choosing any other kind replaces it. sru-59 is modified
to say so.

### D4. Release and the released row's Configure go with the wizard

A Blacklisted record is created by Ignore (wizard) or Release (row). It returns
to Active only through the wizard form, which the released row's Configure
opens. The spec says Release keeps the profile "as dormant reconfiguration seed
data". With the wizard page removed, keeping Release would leave a control
whose only exit deletes the record. Neither story uses it. Rebuilding a path
back without the wizard would be new work no story asks for. So Release, the
released row's Configure, and Ignore all go, and nothing creates a Blacklisted
record any more.

Configurations saved before this change can hold Blacklisted records, so the
disposition, its dormant config, its persistence and its reconciliation stay
(load-bearing for existing data). A released row renders as today (Released
badge, stored port labels) and offers Delete. `DeleteController` refuses a
non-Active record today, and `RemoveFromBlacklist` is the same removal with
the opposite guard. `DeleteController` therefore accepts either disposition,
and `RemoveFromBlacklist`, `Actions::kControllerRemoveBlacklist` and
`NodeIds::ControllerRemoveBlacklist` go. The released row's Delete dispatches
`Actions::kControllerDelete`.

Removing Release shortens an active row's second line. Removing Configure and
Reclaim shortens a released row's. The device label keeps a 308 px floor, so
line one now sets the row's width, which stays what it was. The header's
`static_assert`s guard the line that now sets the row: line two must not grow
past line one, on active and released rows alike. The row still fits the
narrowest host (`TestControllersRowFitsWithinFroggersNarrowestHost`), and the
library device names still fit the label (`RunDeviceLabelWidthCheck`).

`hasCompleteEndpointPair` has no reader once Release is gone, so it goes too.

`CommitLifecycleAction` builds its own view model without the page's registry.
So Restore on a row created from an application's preset reports "Refused:
restoring requires a resolved preset", which makes sru-60's "choosing it SHALL
regenerate" false for every frogg3rs preset. Restore is outside both stories,
and `frogg3rs-transport-and-shift-ux` fixes it, so this change leaves it
alone.

### D5. Configuration forms stay as generation input only

`InstallDescriptorProfile` (add row, Restore, `SlotMatchesWizardProfile`)
creates a wizard, takes `ConfigForm(std::nullopt)` and calls `GenerateProfile`.
Forms therefore stay as the input that generation reads. Their user-interface
half has no caller once the wizard page is gone: `ui::Surface` membership,
`BuildTree`, `BuildSubtree`, `DispatchAction`, `SetActionHandler`, the Twister
form's layout (`TwisterFormLayout`), reconfigure seeding from an existing
profile, and `ReconfigureWarning`. Each is deleted after its invocations are
traced to none: bare name, qualified name, virtual dispatch, and build files.
The trace is the proof; no new test or gate is added for it. `Validate` stays,
because `GenerateProfile` still calls it.

`ConfigForm`'s seed parameter exists only for reconfigure seeding, so
`ConfigForm()` takes no argument. frogg3rs's one caller,
`FroggersControllersPageTests.cpp` (`ConfigForm(std::nullopt)`), changes with
the carry change. Two more pieces go, each after its trace to no caller:
- `ExtractMfTwisterWizardSeed`;
- the helpers in `ControllerWizard.cpp`'s anonymous namespace that only
  removed code reaches.

`RuntimeMainComponent`'s `IsControllersAction` stops accepting the
`controller-wizard.` prefix, because nothing emits such an action.

### D6. The library Twister wizard keeps its default

An application without its own Twister preset (the miniapp) still gets the
library MIDI Fighter Twister descriptor: one encoder slot of 0 and six side
buttons defaulting to Hold Reset, Hold Random, Hold Random Mod, Next Bank,
Start, and Previous Bank. Add installs that default, and the player edits it
on the row, exactly like an application preset.

### D7. A relaunch restores the setup the player last had

At launch, `Engine::Initialize` first loads the runtime configuration. It then
loads the newest saved patch version across all patches (`LatestPatchDirectory`,
which compares version file names built from the save time). When an
application's catalog sets `patchCarriesMappings` (frogg3rs does), that patch's
instrument replaces the one the runtime configuration restored, on the first
`MessageThreadTick`. A patch holds the setup as it was when it was saved, so
every Controllers edit made since then is lost on the next visit, even though
`Commit` saved it.

So a relaunch works this way:
- **The runtime configuration holds the controller setup across a relaunch.**
  The patch loaded at launch restores its parameters and leaves the instrument
  the runtime configuration restored. Edits made on the Controllers page survive
  without the player saving a patch.
- **Opening a patch from the File page applies it as today**, including its
  instrument under `patchCarriesMappings`. Once that instrument is applied, the
  runtime configuration is saved, so the next launch keeps it.
- **Launch reopens the patch version the player last opened or saved.** Opening,
  Save, Save As and Save As (overwrite) record that version file in the runtime
  configuration, relative to the patches root, and save the configuration. New
  records that no patch is open, and the next launch then opens none.
- **Without a record, launch behaves as today once, then records.** A
  configuration written before this change has no record. Until this change, Add
  never saved the configuration, so for many players the controller setup lives
  only in their saved patch. On that first launch, the engine opens the newest
  saved version as today, including its instrument under
  `patchCarriesMappings`. It then records that version and saves the
  configuration, and every later launch follows the rules above.
- **A recorded file that no longer exists falls back.** Launch opens the newest
  saved version, restoring its sound only, because the configuration already
  holds the setup. It records that version.

Sound changes the player never saved are lost on relaunch, as they are today.

In the browser, a saved configuration reaches IndexedDB only when the runtime
reports persistence dirty. Today three places set that flag: the page's save
callback, the services' `SaveRuntimeConfiguration`, and a patch write seen in
`MessageTick`. The saves this decision adds happen inside the engine, after a
patch opens or on New, so none of those three sees them. Every runtime
configuration write the engine makes therefore marks browser persistence dirty,
through one signal the browser runtime reads on each tick. The places that set
the flag separately are then traced, and each one the new signal makes
redundant goes.

### D8. Revert goes

With a patch open, the File page's Revert reloads the newest saved version of
that patch's folder (`PatchManager::RevertPatch`). Load does the same when the
player picks that patch. With no patch open, Revert runs `NewPatch`, which is
exactly what New does. Revert serves no story that Load and New do not, so these
go:
- the Revert button, its action and its node id;
- the Revert branches in `RuntimeFileService` and `juce/RuntimePagesJuce.hpp`;
- each host's `revertPatch` callback, and `Runtime::RevertPatch`;
- `PatchManager::RevertPatch`.

`PatchManager::RevertPatch` also has callers in `SynthRig.hpp`, `engine_tests.cpp`,
`rig_tests.cpp` and `parameter_modulation_tests.cpp`. Each caller is classified:
- a caller that uses it to reload the open patch or to reset calls `LoadPatch`
  or `NewPatch` instead;
- a test whose subject is Revert itself goes with it.

The requirements that name Revert are updated: sru-6, a sru-15 scenario, sar-13,
sprs-3, spp-6 and spp-8.

## Risks

- Upstream loses two features its author built, the Configuration Wizard and
  Revert. The pull request states the two stories and what each measured.
- On the desktop host, every finished field entry and every other committed
  Controllers edit now writes the runtime configuration. A save failure there
  surfaces as a status, as lifecycle saves do today.
- A relaunch now reopens the patch version last opened or saved, not the newest
  saved version across all patches. This changes which sound every Sheaf
  application starts with once a player has opened an older patch.
