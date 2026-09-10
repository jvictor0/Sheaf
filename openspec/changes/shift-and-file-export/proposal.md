# Proposal — `shift-and-file-export`

This change stacks on `app-midi-catalog` (jvictor0/Sheaf#13) and
`rework-controllers-block-editing`, whose deltas its own modify; it is
delivered as the next pull request from the fork against upstream `main`.
Paths are relative to `projects/synth/`, line numbers are reads of
`b50cca18`. Its spec deltas are in this change's `specs/`.

## Why

Two seams the frogg3rs change needs and the library lacks. The first is
Shift. The second is a way for an app to hand a file to whatever host it
runs on: frogg3rs records audio, and in the browser the recording is
captured and dropped, because the app's finished-recording callback is a
seam only its JUCE main knows how to fill (the browser runtime is generic,
`browser/src/build-browser-apps.mjs`, and binds only the app type,
`include/synth/browser/BrowserAppEntry.hpp:8-16`). The runtime has no way
for an app to say "here is a file, save it" that works on both hosts.

**Shift.** An app with more buttons on screen than a controller has under hand needs a
Shift: a held button that gives every other button a second job. The
library has one held modifier today, Hold Drill, and it shows the shape:
a message kind whose press and release flip per-profile state in the
system-button processor and push nothing to the bus
(`src/MidiController.cpp:943-957`). It has no notion of a button doing a
different thing while a modifier is held. The operator ruled that the
second job belongs to the mapping, editable per row on the Controllers
page, not to a fixed table in the app.

## What Changes

- **A `Shift` message kind**, `MessageIn::Type::Shift` and
  `UISystemMessage::Shift`, appended after `HoldDrill` in both enums so no
  ordinal moves, with `MessageIn::Shift(timestamp, held)` shaped like
  `MessageIn::HoldDrill` (`src/ParameterModulation.cpp:4071-4078`). The kind
  visits every switch Hold Drill visits; the family is enumerated below.
- **A shifted press on the association.**
  `MidiControllerSystemMessageAssociation` gains `std::optional<MessageIn>
  shiftedPress`, `std::string shiftedAppAction`, `std::string
  shiftedAppActionValue`. The processor's trimmed copy
  (`SystemButtonMidiAssociation`, `include/synth/MidiController.hpp:368-373`)
  gains `shiftedPress` and the profile builder copies it
  (`src/MidiController.cpp:2990-2996`).
- **Per-profile Shift state.** A `ShiftState { bool held; }` owned by the
  profile result beside `holdDrill` (`include/synth/MidiController.hpp:937`),
  created at `src/MidiController.cpp:2960` and handed to the system-button
  processor only; the encoder processor does not read it.
- **Processor behaviour.** A press whose kind is Shift sets held; its release
  clears held; neither pushes. Any other press pushes `*shiftedPress` when
  Shift is held and the association has one, else `press`. A release is
  unchanged. Shift and Hold Drill are independent: both can be held.
- **Persistence.** `shiftedPress` serializes beside `release`, null when
  absent; `shiftedAppAction`/`shiftedAppActionValue` are written only when
  the shifted press is `AppAction`. A document without the keys loads with
  no shifted press, the way `openSysEx` did (`:2750-2753`).
- **Resolve.** `ResolveAppActionsAgainstCatalog` (`include/synth/Engine.hpp:
  1040-1052`) resolves a shifted `AppAction` press by its own name/value pair.
  An unresolvable shifted press is cleared on the copy; the row itself is
  kept, so a preset whose shifted job a later catalog drops keeps its
  ordinary job.
- **Controllers page.** A new `MidiMappingRowVM::Field::ShiftAction`,
  declared last in `Field` (after `GridYMax`; a field's token is the
  enumerator's integer, `include/synth/ControllersPageUI.hpp:761-777`, so
  appending keeps every existing token), on every individual
  system-message row whose own kind is neither Shift nor Hold Drill,
  appended after the argument field (`src/MidiConfigViewModel.cpp:
  1063-1067`), 150 px wide (`include/synth/ControllersPageUI.hpp:561-563`),
  column header "Shift". Its choices are none, then every entry of the row
  dropdown whose kind takes no argument (`UISystemMessageHasArg`,
  `src/MidiConfigViewModel.cpp:97-127`), read through a new
  `ShiftChoiceIndex` shaped like `UISystemMessageIndex` (`:1828-1856`);
  the choice list is derived once, where `SetMessageCatalog` stores the
  row dropdown (`:541-543`), not per field per frame and
  committed through the `MessageKind` edit path's shape (`:2690-2705`)
  against `shiftedPress`. The row dropdown offers Shift the way it offers
  Hold Drill (`:506-509`). The row label gains "shift on"/"shift off"
  (`:726-728`); the sort key treats Shift like Hold Drill
  (`src/MidiConfigBlocks.cpp:119-122`).
- **A file export seam.** `synth::FileExport { std::string fileName;
  std::string mediaType; std::vector<std::uint8_t> bytes; std::string
  note; }` and an optional app hook `HasFileExports<App>`:
  `app.TakePendingFileExport()` returning `std::optional<FileExport>`,
  declared beside the other optional hooks (`include/synth/AppConcepts.hpp:
  69-82`). `Engine` gains `SetFileExportHandler(std::function<void(FileExport)>)`
  and, at the end of `MessageThreadTick` after the bus drain
  (`include/synth/Engine.hpp:497-535`), takes every pending export and hands
  it to the handler; with no handler installed it logs the drop by name. An
  app without the hook sees nothing.
- **The browser saves an export as a download.** The browser runtime
  installs a handler that queues exports (`include/synth/browser/
  BrowserRuntime.hpp:713-723`, beside the persistence flag), and a new ABI
  entry `synth_browser_dequeue_file_export` returns the next one in the
  pointer-plus-size shape `synth_browser_build_ui_frame` uses
  (`browser/src/worker.ts:340-345`), name and media type as UTF-8 fields.
  `BrowserRuntimeWorker`'s `message-tick` case (`worker.ts:619-622`) then
  drains the exports and emits each through its `emitStatus` callback as
  `{ type: "file-export", fileName, mediaType, bytes }`, the way
  `page-status` is emitted (`:46`, `:478`, `:593`). Both runtime clients
  are served by that one drain: the direct client (`main.ts:89-121`) runs
  the worker class in the page realm and hands `emitStatus` straight to
  its status handlers; the worker client (`:124-155`) receives it as a
  worker message and forwards it to its status handlers the way it
  forwards `page-status` (`:129-131`), with its reply listener ignoring it
  the same way (`:136-138`). `SynthBrowserApp`'s status subscription
  (`:183`) turns a `file-export` into a Blob, an object URL and an anchor
  click with `download` set to the name, revokes the URL, and passes every
  other status to `renderStatus` as today. The generic-runtime check
  (`browser/tests/check-generic-runtime.mjs`) scans these files for app
  identity and forbidden audio fallbacks; the new code names no app. The
  standalone needs nothing from the library: its main already reaches the
  engine (`frogg3rs app/FroggersMain.cpp:177`) and installs its own
  dialog as the handler.
- **Sweep finding, fixed here: analog-ranged actions leave the row
  dropdown.** `MakeUISystemMessageChoices` (`src/MidiConfigViewModel.cpp:
  514-522`) offers every catalog action as a button target, BPM included; a
  button so mapped dispatches the range minimum on every press
  (`include/synth/Engine.hpp:508-511` with message value 0). The dropdown now
  skips analog-ranged actions; `MakeAnalogAppActionChoices` (`:526-539`)
  owns them.
- **Sweep finding, fixed here: one combo emitter.** The page's field
  emitter has three near-identical combo branches, message kind, app action
  and encoder mode (`include/synth/ControllersPageUI.hpp:2666-2725`), differing
  only in the option source and the current-index read. The Shift combo
  would be a fourth. They become one branch taking options and current
  index.

## The Hold Drill family, by operand

Every site is a site Shift visits; the executor reports found versus
changed against this list, zeros included.

| file | sites | what they are |
|---|---|---|
| `src/MidiController.cpp` | 25 | kind name and parse (`:233-234`, `:288-289`); processor state and press/release handling (`:676-681`, `:708-714`, `:918-921`, `:943-957`); feedback exclusion (`:1856`); MessageIn JSON (`:2411`, `:2483`); profile creation and hand-off (`:2960-2961`, `:2982`, `:2998`) |
| `src/MidiConfigViewModel.cpp` | 12 | arg switches (`:50`, `:92`, `:123`), kind mapping (`:188-189`), press and release builders (`:241-242`, `:276-277`), the library-produced choice (`:507-508`), row label (`:726`) |
| `src/ParameterModulation.cpp` | 3 | factory (`:4071`), bus `Apply` no-op (`:4239`) |
| `src/ControllerWizard.cpp` | 2 | built-in Twister form switches (`:185`, `:396`) |
| `src/MidiConfigBlocks.cpp` | 2 | kind-count comment (`:26`), sort key (`:119`) |
| `include/synth/MidiController.hpp` | 6 | state struct and constructor parameters (`:249-275`, `:383-394`), profile result (`:937`) |
| `include/synth/ParameterModulation.hpp` | 2 | enumerator (`:978`), factory (`:1044`) |
| `include/synth/MidiConfigViewModel.hpp` | 2 | enumerator (`:227`), comment (`:266`) |
| `include/synth/MidiConfigBlocks.hpp` | 1 | kind-count comment (`:75`) |

The encoder-processor sites (`:676-681`, `:708-714`, `:2982`) are Hold
Drill's alone; Shift is consumed where it is set. Both counts are reported.

## Impact

- `include/synth/MidiController.hpp`, `ParameterModulation.hpp`,
  `MidiConfigViewModel.hpp`, `MidiConfigBlocks.hpp`, `ControllersPageUI.hpp`,
  `Engine.hpp`, `AppConcepts.hpp`, `browser/BrowserRuntime.hpp`.
- `src/MidiController.cpp`, `ParameterModulation.cpp`,
  `MidiConfigViewModel.cpp`, `MidiConfigBlocks.cpp`, `ControllerWizard.cpp`;
  `browser/cpp/BrowserRuntimeAbi.cpp`; `browser/src/worker.ts`, `main.ts`,
  `protocol.ts`.
- `tests/instrument_tests.cpp`, `engine_tests.cpp`, `viewmodel_tests.cpp`,
  `portable_ui_tests.cpp`, `controllers_page_ui_tests.cpp`,
  `browser_runtime_contract_tests.cpp`; `browser/tests/fixtures/cpp/
  FakeBrowserApp.hpp` and one new Playwright spec under `browser/tests/`.
- `openspec/changes/shift-and-file-export/`: the Sheaf-side change
  directory task 1.S5.1 creates (proposal, tasks, and `specs/` moved from
  this change's `sheaf-specs/`), in the shape of `app-midi-catalog/`;
  deltas to smi-1, smi-2, smi-8, sru-59, sru-15, sru-16 as they stand in
  the two active changes, and new smi-16, sar-33, sbw-12.

Not touched: `include/synth/ControllersPageUI.hpp`'s width constants; the
fits gate decides the row (below). `ProfileConfigValidForKind`
(`src/MidiController.cpp:3388`) constrains sections and addresses, not a
message's presence, and needs no change.

## Data flow after the change

Shift button down → CC above zero → processor finds the association, kind
Shift → `shift_->held = true`, nothing pushed. Another button down → its
association has a shifted press → that message is pushed with the same
stamp `press` would have had; if it is `AppAction`, its index was resolved
on the rebuilt copy by the shifted name/value pair, and the engine
dispatches the catalog entry at that index exactly as for an ordinary press
(`include/synth/Engine.hpp:504-514`). Shift button up → held cleared.

## The row fits

A Generic system row is address type 90, channel 66, CC 66, kind 150,
argument 74 and four gaps of 4 (`include/synth/ControllersPageUI.hpp:
556-600`, `:480`, `:2852-2862`): 462 px. With the Shift combo, 616 px. The
gate `TestControllersRowFitsWithinFroggersNarrowestHost`
(`tests/portable_ui_tests.cpp:3010`) builds every open state at 900 px and
has a proven positive control; it is re-run, not reasoned about. Its
24-choice assertion (`:3376-3377`) counts a hand-built fixture catalog
(`:3070-3112`): five kept kinds, ten plain actions, six banks, two scenes
and one analog-ranged BPM. The fixture mirrors Froggers' catalog, so task
1.S4.4 adds `Shift` to its `libraryKinds` as task 2.1 does to the app's;
BPM leaves the dropdown and the count stays 24 (six kinds, eighteen
actions).

## Data flow of an export

App stops a recording on the message thread → it queues one `FileExport` →
the engine's next tick takes it and calls the handler. Standalone: the
handler is the app main's dialog; the file is written where the operator
chooses. Browser: the runtime's handler appends to a queue; the worker's
tick dequeues, copies name, media type and bytes out of the wasm heap, and
posts them to the page with the bytes transferred; the page offers the
download. Nothing is persisted in the runtime's own storage and nothing
touches the audio thread.
