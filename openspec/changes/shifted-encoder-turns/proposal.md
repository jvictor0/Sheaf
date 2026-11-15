# Proposal — `shifted-encoder-turns`

This change modifies smi-16 and sru-15 as they stand in the active change
`shift-and-file-export` (jvictor0/Sheaf#14), and the promoted sru-10; it adds
smi-17 and sru-66 (`fold-controller-wizard-into-add-row`, jvictor0/Sheaf#19,
added sru-64 and sru-65). It is based on `f6266560`, the current tip of
`fold-controller-wizard-into-add-row` (#19) on the fork: one commit past that
branch's "Record the delivery" commit `e8894727`, still unmerged upstream;
task 9 carried it from the previous base `e8894727` onto `f6266560`. It is
delivered as the next pull request from the fork against upstream `main`,
after the open ones. Paths are
relative to `projects/synth/`, and code is named by symbol. Its spec deltas
are in this change's `specs/`.

sru-15 names two requirements in Sheaf: the promoted "Controllers page:
DOM-friendly semantic presentation", which `fold-controller-wizard-into-add-row`
modifies, and "Controllers page: system message kind and argument editors",
which the active change `rework-controllers-block-editing` adds and
`shift-and-file-export` modifies. This change modifies the second.

## Why

On the base, Shift is a per-profile `ShiftState` that only the system-button
processor reads. `EncoderMidiInProcessor` takes a `HoldDrillState` and no
`ShiftState`, so a held Shift changes nothing about a knob. Scene blend
reaches the library only as the absolute `MessageIn::SetSceneBlend`, from an
analog CC. The Twister's encoders are relative (Enc 3FH/41H). Nothing in the
library turns a relative knob into a blend move, with or without Shift.

The Twister cannot supply this from the device side either. Its manual
(`https://s3.amazonaws.com/MF_Support_Docs/Midi+Fighter+Twister+User+Guide.pdf`,
pages 12 and 14) offers two shift features: an encoder switch set to Shift
Encoder Hold or Toggle makes that encoder's own turns send a second value,
and a side button set to Shift Page A or B changes what the encoder
switches send. A side button cannot shift encoder turns, and a preset
needing its Shift side button to stay CC Hold for other shifted button jobs
cannot spend it on Shift Page A/B either.

## What Changes

The addition is general: a held Shift gives a knob turn a second job, as it
already gives a button a second job, and the job belongs to the turn's own
mapping, editable per row on the Controllers page. That is the rule the
operator set for button Shift.

- **A scene-blend increment message.** NEW `MessageIn::Type::SceneBlendIncDec`,
  appended after `Shift` so no ordinal moves, with a
  `MessageIn::SceneBlendIncDec(timestamp, delta)` factory. `MessageInBus::Apply`
  hands it to NEW `ParameterManager::IncDecSceneBlend(delta)`, which adds to
  the blend and clamps to 0..1 beside `SetSceneBlend`. It is applied on the
  audio thread, so fast turns never lose increments to a stale read. The kind
  has a case at every site of the `SetSceneBlend` family listed under
  Evidence except the test catalog table in `tests/viewmodel_tests.cpp`,
  which lists row-dropdown kinds, and the increment is not one.
- **A shifted job on the turn mapping.** `EncoderMidiMapping` carries
  `shiftedJob`, NEW `EncoderShiftedJob` (`None`, `SceneBlend`). It
  serializes as `"shiftedJob": "sceneBlend"` only when set. A missing key
  reads as None and an unknown value fails the load. A push mapping carrying
  one is an invalid profile.
- **The encoder processor reads Shift.** `EncoderMidiInProcessor` takes the
  profile's `ShiftState`, and the profile builder passes it as it passes
  `holdDrill`. `EncoderMidiInProcessor::Process` handles a turn in this
  order: Hold Drill as on the base; then, if Shift is held and the mapping's
  shifted job is Scene blend, a relative turn pushes `SceneBlendIncDec` with
  the decoded delta and an Absolute turn pushes `SetSceneBlend` with the
  normalized value; otherwise the turn is handled as on the base. The
  `ShiftState` comment names both processors that read it.
- **Controllers page.** An Individual encoder-turn row has the Shift field
  (`Field::ShiftAction`, header "Shift"), offering none and Scene Blend and
  committing through the row's existing flush path. The page's
  `Field::ShiftAction` combo branch in `ControllersPageUI.hpp` fills a turn
  row's options from NEW `EncoderShiftedJobCatalog()` and its selection
  from NEW `MidiConfigViewModel::EncoderTurnShiftedJobIndex`; a system row
  still uses `ShiftCatalog()` and `ShiftChoiceIndex()`. `BuildSectionRows` and
  `MidiConfigViewModel::GroupColumnFields` read a turn row's fields from one
  NEW `EncoderTurnEditableFields` function, as they read a system row's from
  `SystemRowEditableFields`. `ReconstructEncoderBlocks` never folds a turn
  with a shifted job into a block, so on a Twister row whose last turn is
  shifted the encoder section reads as one 15-turn block plus that turn's
  own row showing Shift = Scene Blend. A block expands to turns with no
  shifted job. Push rows have no Shift field.
- **Sweep finding, fixed here.** On the base, `SystemRowEditableFields` adds
  the Shift field to every system row. The row dropdown offers Shift only
  when the app's catalog lists it (`UISystemMessageCatalog()` has no Shift
  entry), so an app without a catalog, such as braid-4 or the miniapp, shows
  a Shift column that nothing can ever hold. NEW `MessageCatalogOffersShift`
  answers whether the catalog offers Shift, and the Shift field on system
  rows and on turn rows appears only when it does.
- **Found during execution, fixed here: Restore refuses every app preset.**
  On the base, the page's lifecycle actions (Rename, Delete, Restore) run
  through `ControllersPageSurface::CommitLifecycleAction`, which builds a
  throwaway `MidiConfigViewModel` and calls `Rebuild` on it without the
  message catalog, analog action catalog or layouts the surface's own view
  model receives from `ControllersPageCallbacks` in the constructor. With no
  layouts set, `MidiConfigViewModel::Layouts` falls back to
  `MakeControllerWizardRegistry(MidiAppCatalog{})`, the library's own
  registry, which holds none of an app's presets, so
  `MidiConfigViewModel::RestoreController` never resolves an app preset's
  wizard id and answers "restoring requires a resolved preset". sru-60 says
  choosing Restore regenerates the resolved preset's profile; its tests drive
  the library's own Twister preset, which the fallback registry holds.
  `CommitLifecycleAction` now configures its throwaway view model exactly as
  the surface's own is configured, through one NEW member function,
  `ControllersPageSurface::ConfigureViewModel`, which both the constructor
  and `CommitLifecycleAction` call, so the two cannot drift again.
  `HandleRestoreController` also drops that controller's cached section rows
  on the surface's own view model, through the public NEW
  `MidiConfigViewModel::NoteControllerConfigReplaced` beside
  `NoteControllerRenamed`: an open section keeps its rows until its
  controller is removed or the section is collapsed, so without this the
  page keeps showing the pre-Restore rows and the next encoder edit flushes
  them back over the restored mappings.

## Data flow

**Shift + Crunchy** (frogg3rs' Twister preset, whose position-15 turn moves
Crunchy and carries Scene blend). Shift side button down (ch3 CC13, 127) →
encoder processor finds no turn or push → passes through → system-button
processor → `shift_->held = true`. Encoder 16 turned clockwise (ch0 CC for
position 15, value 65) → encoder processor finds the turn → Hold Drill not
held → Shift held and shifted job Scene blend → `DecodeDelta` gives +1 turn
step → `SceneBlendIncDec` pushed → audio thread `MessageInBus::Apply` →
`IncDecSceneBlend` → blend rises, Crunchy untouched → UI state mirrors the
blend and the on-screen slider moves. Shift up → held false → the next turn
pushes `ParamIncDec` for Crunchy.

## Delivery

Every Shift change here is additive: a new message kind appended after the
last enumerator, a new optional field whose absence reads as the base's
behaviour, a new processor input defaulting to null, and a Controllers-page
field that appears only when the app's catalog offers Shift. No existing
message, mapping or saved document changes meaning. A saved instrument or
patch without `shiftedJob` loads exactly as before, and an app that never
sets a shifted job sees no change. The one removal is the sweep finding:
apps that cannot map Shift lose a Shift column that could never fire. The
Restore fix is not additive, and is not gated on Shift: it corrects
`CommitLifecycleAction`'s throwaway view model for every app with its own
presets, on every controller kind, whether or not that app's catalog offers
Shift. It changes no saved document; a controller record's own fields are
untouched. The test for each edit here is whether an app author who has
never heard of frogg3rs would want it. Shift on knobs passes that test, and
so does a Restore that resolves.

This work is done on the fork on a branch based on the stack tip `f6266560`
(`fold-controller-wizard-into-add-row`, jvictor0/Sheaf#19) and pushed to the
fork (`fork` remote, `daguilarc:shifted-encoder-turns`). It is opened as the
next pull request against jvictor0/Sheaf `main`, after the open ones (#14,
#17, #18, #19); nothing in the stack is merged into either Sheaf `main` by
this change. smi-16 and this change's sru-15 exist only in open changes, so
this change stays active until upstream merges the stack. The branch also
carries a second openspec change, `block-end-fields-show-the-last-control`,
which ships in the same pull request. The pull request description carries
step-by-step testing instructions for Shift + knob on a Twister and for that
change's block end fields.

## Evidence

On the base, Shift reaches only the button processor. The encoder
processor's constructor takes a `HoldDrillState` and no `ShiftState`; only
the system-button processor's takes one:

```
$ git grep -n -E "ShiftState\*|HoldDrillState\*" e8894727 -- projects/synth/include projects/synth/src
e8894727:projects/synth/include/synth/MidiController.hpp:265:                           HoldDrillState* holdDrill = nullptr);
e8894727:projects/synth/include/synth/MidiController.hpp:281:    HoldDrillState* holdDrill_ = nullptr;
e8894727:projects/synth/include/synth/MidiController.hpp:390:                                HoldDrillState* holdDrill = nullptr, ShiftState* shift = nullptr);
e8894727:projects/synth/include/synth/MidiController.hpp:401:    HoldDrillState* holdDrill_ = nullptr;
e8894727:projects/synth/include/synth/MidiController.hpp:402:    ShiftState* shift_ = nullptr;
e8894727:projects/synth/src/MidiController.cpp:681:                                               HoldDrillState* holdDrill)
e8894727:projects/synth/src/MidiController.cpp:923:                                                          HoldDrillState* holdDrill, ShiftState* shift)
e8894727:projects/synth/src/MidiController.cpp:3028:    HoldDrillState* const holdDrill = result.holdDrill.get();
e8894727:projects/synth/src/MidiController.cpp:3030:    ShiftState* const shift = result.shift.get();
```

The family `SceneBlendIncDec` visits: every switch case, name mapping and
factory for its sibling `SetSceneBlend` on the base (lines cut at 150
characters):

```
$ git grep -n -E "case MessageIn::Type::SetSceneBlend|\"setSceneBlend\"|Type::SetSceneBlend;|Type::SetSceneBlend," e8894727 -- projects/synth/src projects/synth/include projects/synth/tests/blocks_tests.cpp projects/synth/tests/viewmodel_tests.cpp | cut -c1-150
e8894727:projects/synth/src/MidiConfigBlocks.cpp:92:        case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/src/MidiConfigViewModel.cpp:43:        case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/src/MidiConfigViewModel.cpp:86:        case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/src/MidiConfigViewModel.cpp:181:        case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/src/MidiConfigViewModel.cpp:735:        case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/src/MidiController.cpp:222:    case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/src/MidiController.cpp:223:        return "setSceneBlend";
e8894727:projects/synth/src/MidiController.cpp:279:    } else if (value == "setSceneBlend") {
e8894727:projects/synth/src/MidiController.cpp:280:        type = MessageIn::Type::SetSceneBlend;
e8894727:projects/synth/src/MidiController.cpp:1870:    case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/src/MidiController.cpp:2428:    case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/src/MidiController.cpp:2501:    case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/src/ParameterModulation.cpp:4017:    message.type = Type::SetSceneBlend;
e8894727:projects/synth/src/ParameterModulation.cpp:4212:    case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/tests/blocks_tests.cpp:139:        case MessageIn::Type::SetSceneBlend:
e8894727:projects/synth/tests/viewmodel_tests.cpp:3943:        {UISystemMessage::SetSceneBlend, synth::MessageIn::Type::SetSceneBlend, false, false,
```

Found 16 lines at 14 sites; the change adds a `SceneBlendIncDec` counterpart
at 13 of them and not at the `tests/viewmodel_tests.cpp` catalog row, since
the increment is no row-dropdown kind:

```
$ git grep -n -E "case MessageIn::Type::SceneBlendIncDec|Type::SceneBlendIncDec;|MessageIn MessageIn::SceneBlendIncDec" HEAD -- projects/synth/src projects/synth/tests/blocks_tests.cpp projects/synth/tests/viewmodel_tests.cpp | cut -c1-150
HEAD:projects/synth/src/MidiConfigBlocks.cpp:94:        case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiConfigViewModel.cpp:52:        case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiConfigViewModel.cpp:96:        case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiConfigViewModel.cpp:197:        case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiConfigViewModel.cpp:781:        case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiController.cpp:238:    case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiController.cpp:298:        type = MessageIn::Type::SceneBlendIncDec;
HEAD:projects/synth/src/MidiController.cpp:1889:    case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiController.cpp:2456:    case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiController.cpp:2530:    case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/ParameterModulation.cpp:4093:MessageIn MessageIn::SceneBlendIncDec(std::uint64_t timestamp, float delta) {
HEAD:projects/synth/src/ParameterModulation.cpp:4096:    message.type = Type::SceneBlendIncDec;
HEAD:projects/synth/src/ParameterModulation.cpp:4229:    case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/tests/blocks_tests.cpp:156:        case MessageIn::Type::SceneBlendIncDec:
```

The Twister's turns are one block on the base and the position-15 turn is
CC 15: `EncoderMidiInConfig::TwisterDefault` returns `RowMajorInputDefault`,
which maps position `p` to channel 0, CC `EncoderPositionToCC(p)`, which
returns `position % 16`, and `ReconstructEncoderBlocks` extends a run while
slot and channel hold and position and CC both advance by one.

No Sheaf app defines a MIDI catalog; the only definition outside the engine's
own accessor is a test fixture, so an app with no catalog of its own builds
its row dropdown from `UISystemMessageCatalog()`, which has no Shift:

```
$ git grep -n -E "MidiAppCatalog MidiCatalog\(|MidiCatalog\(\) (const )?\{|static .*MidiCatalog\(" e8894727
e8894727:projects/synth/include/synth/Engine.hpp:698:    const MidiAppCatalog& MidiCatalog() const { return midiCatalog_; }
e8894727:projects/synth/tests/engine_tests.cpp:3044:    synth::MidiAppCatalog MidiCatalog() const { return catalog; }
```

## Capabilities

### Modified Capabilities
- `synth-midi-instrument`: smi-16 modified, smi-17 added.
- `synth-runtime-ui`: sru-10 and sru-15 modified, sru-66 added.

## Impact

- `include/synth/ParameterModulation.hpp`, `include/synth/MidiController.hpp`,
  `include/synth/MidiConfigViewModel.hpp`, `include/synth/MidiConfigBlocks.hpp`
  (the `SystemMessageSortKey` comment that counts the message kinds),
  `src/ParameterModulation.cpp`, `src/MidiController.cpp`,
  `src/MidiConfigViewModel.cpp`, `src/MidiConfigBlocks.cpp`,
  and `include/synth/ControllersPageUI.hpp` (its Shift combo branch, and
  `ConfigureViewModel`, `CommitLifecycleAction` and `HandleRestoreController`,
  the lifecycle actions' view model).
- Tests: `tests/instrument_tests.cpp`, `tests/parameter_modulation_tests.cpp`,
  `tests/viewmodel_tests.cpp`, `tests/blocks_tests.cpp`,
  `tests/portable_ui_tests.cpp`, `tests/controllers_page_ui_tests.cpp`.
