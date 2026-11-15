# Proposal — `shifted-turn-moves-the-tempo`

This change modifies smi-17 and sru-66 as they stand in the active change
`shifted-encoder-turns` (jvictor0/Sheaf#20), which added both, and smi-13 as
it stands in the active change `app-midi-catalog`. It is based on `40aea92d`,
the current tip of the `shifted-encoder-turns` branch on the fork, which
frogg3rs `main` at `424d7c8` pins. Paths are relative to `projects/synth/`,
and code is named by symbol. Its spec deltas are in this change's `specs/`.

## Why

A held Shift gives a knob turn a second job, and today the one job on offer
is Scene blend. Tempo is the other thing a player reaches for mid-take, and
it has no knob: an app's tempo is driven from a fader, absolutely, through
the app-action catalog, and nothing in the library turns a relative knob into
a tempo move.

Tempo is not a parameter, so the parameter path cannot carry it. Its value
lives in `MasterClock::activeBpm_`, reached through `MasterClock::SetTempoBpm`
and `MasterClock::TempoBpm`, and the library has no message that touches
either. An app's own MIDI route to tempo is the app-action route: a control's
normalized value becomes `MessageIn::AppAction`, the audio thread forwards it
to the app-action output bus, and the message thread rescales it into the
catalog action's declared analog range and dispatches it to the app's surface
as a `ui::Action` (smi-13). That route is absolute by construction — it
carries a position, never a step.

## What Changes

- **The message bus can move the master clock.** `MessageInBus` takes a
  `MasterClock*` and a tempo range through NEW
  `MessageInBus::SetTempoClock(MasterClock*, float minimumBpm, float
  maximumBpm)`, beside `SetGridManager` and `SetAppActionOut`, and `Engine`
  wires it on both `uiBus_` and `midiBus_` exactly where it wires those two.
  `ParameterModulation.hpp` forward-declares `MasterClock` and
  `ParameterModulation.cpp` includes `synth/MasterClock.hpp`; the include does
  not run the other way, so no cycle appears.
- **Where the range comes from.** `MidiAppCatalog` carries NEW `tempoAction`,
  the name of the catalog action that IS the master clock's tempo, in the
  shape `encoderPressAction` already has: a bare action name, matched with an
  empty value. `Engine` resolves it once through the existing
  `FindMidiAppAction` and hands that action's `analogRange` to the buses. An
  app that names no tempo action, names one that does not resolve, or names
  one that declares no analog range leaves the buses with no clock and no
  range, and both tempo messages below are dropped. This keeps the range
  stated once, by the app, in the place the app already states it.
- **Two tempo messages.** NEW `MessageIn::Type::TempoBpmIncDec` and NEW
  `MessageIn::Type::SetTempoBpmNormalized`, appended after `SceneBlendIncDec`
  so no existing ordinal moves, with factories
  `MessageIn::TempoBpmIncDec(timestamp, deltaBpm)` and
  `MessageIn::SetTempoBpmNormalized(timestamp, normalized)`. The first carries
  a step in BPM, the second a position in 0..1; each name says which, because
  a step and a position are different quantities and one unit cannot serve
  both. `MessageInBus::Apply` handles them on the audio thread, where the
  clock's value lives: the increment reads `TempoBpm()`, adds the step, clamps
  to the range and calls `SetTempoBpm`; the set maps `normalized` to
  `minimum + normalized * (maximum - minimum)` and calls `SetTempoBpm`. Both
  kinds get a case at every site of the `SceneBlendIncDec` family listed under
  Evidence.
- **A second shifted job.** `EncoderShiftedJob` gains `TempoBpm` after
  `SceneBlend`. It serializes as `"shiftedJob": "tempoBpm"`, written only when
  set; a missing key still reads as none and an unknown value still fails the
  load. A push mapping carrying it is still an invalid profile, by the check
  that already rejects any shifted job on a push.
- **Dispatch for both encoder modes.** `EncoderMidiInProcessor::Process`
  handles a turn whose shifted job is Tempo, while Shift is held, in the same
  slot in the order Hold Drill / shifted job / ordinary turn: a relative turn
  pushes `TempoBpmIncDec` carrying the turn's detent count multiplied by NEW
  `kTempoBpmPerEncoderDetent`, and an Absolute turn pushes
  `SetTempoBpmNormalized` carrying the turn's normalized value. The shifted
  branch returns before any slot or parameter-bank lookup, so whether the
  turn's own parameter is scoped to one parameter bank makes no difference to
  it.
- **The detent count becomes its own step.** `EncoderMidiInProcessor` gains
  NEW private `DecodeTicks`, returning the signed detent count for a relative
  mode and nothing for Absolute; `DecodeDelta` becomes that count times
  `turnStep`. The tempo branch needs detents, not parameter steps, because
  `turnStep` is a fraction of a parameter's normalized range and tempo has no
  such range. Two callers and one distinct stage.
- **Controllers page.** `EncoderShiftedJobCatalog()` gains "BPM" after "Scene
  Blend", which is the whole of the page change: the turn row's Shift combo
  builds its options from that catalog, `EncoderTurnShiftedJobIndex` returns
  the enumerator's own ordinal, and `ApplyMappingEdit`'s `ShiftAction` case
  validates the typed index against the catalog's size. The three comments
  that name the two choices by number — the index comment on
  `MidiConfigViewModel::EncoderTurnShiftedJobIndex`, the `Field::ShiftAction`
  enumerator's comment in `MidiConfigViewModel.hpp`, and the encoder-turn
  combo builder's comment in `ControllersPageUI.hpp` — are all corrected;
  the last two name the count without naming `EncoderShiftedJob`, so a grep
  for that symbol alone would have missed them.
- **The round-trip scenario's unknown-job example changes.** smi-17's
  round-trip scenario uses `"tempo"` as its example of a shifted job the
  library does not know. With `"tempoBpm"` now a known key, that example
  would read as though tempo were the rejected case, so the scenario and its
  test use `"swing"` instead.

## The tempo step

`kTempoBpmPerEncoderDetent` is 1.0 BPM per encoder detent, linear. Tempo is
named by musicians in whole numbers, so a linear law lands a detent on an
integer where a logarithmic one would land on a fraction the one-decimal
readout would show; 1.0 also matches the on-screen BPM slider's own step,
already what JUCE's `setRange` interval and the browser input's step use, so
the knob and the slider agree on the finest move either can make. The
constant is declared in task 3.

What holds at the tempo boundaries and under load:

- The applied tempo is clamped to the range the app declares, at both ends.
  With frogg3rs' 30 to 300, turning down past 30 leaves the tempo at 30 and
  turning up past 300 leaves it at 300; neither end wraps, refuses the turn,
  or stops the knob from moving back the other way on the next detent.
- A turn's direction is the sign of its detent count, so the same knob turned
  the other way moves the tempo the other way by the same amount.
- The Twister's own acceleration carries through without special handling: a
  fast turn sends a value further from 64, `DecodeTicks` returns the larger
  count, and the step scales with it.
- `MasterClock::SetTempoBpm` refuses while the clock is slaved to external
  MIDI, so a shifted turn moves nothing then and reports nothing. That is the
  same answer the app's own on-screen tempo control gives.
- The read-modify-write happens on the audio thread, where `activeBpm_` lives,
  so a fast turn never loses detents to a stale mirror of the tempo.

## Data flow

**Shift + a tempo-shifted knob, relative.** Shift button down → encoder
processor finds no turn or push → passes through → system-button processor →
`shift_->held = true`. The knob is turned clockwise (Signed7Bit, value 65) →
encoder processor finds the turn → Hold Drill not held → Shift held and
shifted job Tempo → `DecodeTicks` gives +1 → `TempoBpmIncDec(+1 *
kTempoBpmPerEncoderDetent)` pushed → audio thread `MessageInBus::Apply` →
`MasterClock::TempoBpm() + step`, clamped to the app's range →
`MasterClock::SetTempoBpm` → the clock's tempo moves and the knob's own
parameter does not. Shift up → held false → the next turn pushes `ParamIncDec`
for that parameter again.

**What the app sees.** Nothing new. An app that publishes its tempo for its
own surface reads it from `MasterClock::TempoBpm()`, so the on-screen tempo
follows a shifted turn with no app change — in frogg3rs, through the
once-per-block publish at `FroggersAppCore.hpp`'s
`tempoDisplayBpm_.store(context_->masterClock->TempoBpm(), ...)`.

**Against the existing absolute route.** An app's fader still reaches tempo
through the app-action route and its own request path; a shifted turn reaches
it through the bus. In frogg3rs both land on the audio thread inside the same
block — the app drains its pending tempo request in `ProcessFrame` and the
engine drains `midiBus_` in `ProcessBlock` — so the later of the two in a
block wins, and there is no torn value in either case.

## Evidence

The shifted dispatch returns before the encoder mode's slot handling, so a
shifted turn never resolves a slot or a parameter bank:

```
$ sed -n '728,738p' projects/synth/src/MidiController.cpp
            if (shift_ != nullptr && shift_->held && mapping->shiftedJob == EncoderShiftedJob::SceneBlend) {
                if (config_.mode == EncoderMode::Absolute) {
                    Push(MessageIn::SetSceneBlend(NextTimestamp(),
                                                  AbsoluteEncoderByteToNormalized(midi.GetValue())));
                } else if (const std::optional<float> delta = DecodeDelta(midi.GetValue())) {
                    Push(MessageIn::SceneBlendIncDec(NextTimestamp(), *delta));
                }
                return;
            }
            if (config_.mode == EncoderMode::Absolute) {
```

The bus's own translation unit does not mention the clock at all, so the
dependency is new in both directions:

```
$ git grep -c "MasterClock" HEAD -- projects/synth/include/synth/ParameterModulation.hpp projects/synth/src/ParameterModulation.cpp
(exit 1)
```

`SetTempoBpm` applies no range of its own: it rejects a non-finite or
non-positive tempo and refuses while slaved, and otherwise takes any value.
The clamp has to come from somewhere else, and nothing in the library holds a
tempo range today:

```
$ sed -n '963,969p' projects/synth/src/MasterClock.cpp
bool MasterClock::SetTempoBpm(double bpm) noexcept {
    if (!IsFinitePositive(bpm) || syncConfig_.receiveClock) {
        return false;
    }
    double nextQuarterNotesPerSample = pendingQuarterNotesPerSample_;
    if (prepared_) {
        nextQuarterNotesPerSample = bpm / (60.0 * sampleRate_);
```

`MasterClock.hpp` includes no parameter header, so the forward declaration is
enough and no cycle appears:

```
$ grep -n "^#include" projects/synth/include/synth/MasterClock.hpp
6:#include "synth/DspPhasor2Tick.hpp"
8:#include <array>
9:#include <cstddef>
10:#include <cstdint>
11:#include <optional>
```

**The `SceneBlendIncDec` family, and whether each site needs the two tempo
kinds.** Every site here is one the compiler or a reader reaches by message
kind, so the verdict is the same at all of them unless stated:

```
$ git grep -n -E "case MessageIn::Type::SceneBlendIncDec|Type::SceneBlendIncDec[;,]|MessageIn MessageIn::SceneBlendIncDec|SceneBlendIncDec=26|\"sceneBlendIncDec\"" HEAD -- projects/synth/src projects/synth/include projects/synth/tests | cut -c1-150
HEAD:projects/synth/include/synth/MidiConfigBlocks.hpp:75:    // MessageIn::Type's declaration order (ParamIncDec=0 .. HoldDrill=24, Shift=25, SceneBl
HEAD:projects/synth/src/MidiConfigBlocks.cpp:133:        case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiConfigViewModel.cpp:52:        case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiConfigViewModel.cpp:96:        case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiConfigViewModel.cpp:197:        case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiConfigViewModel.cpp:793:        case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiController.cpp:238:    case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiController.cpp:239:        return "sceneBlendIncDec";
HEAD:projects/synth/src/MidiController.cpp:297:    } else if (value == "sceneBlendIncDec") {
HEAD:projects/synth/src/MidiController.cpp:298:        type = MessageIn::Type::SceneBlendIncDec;
HEAD:projects/synth/src/MidiController.cpp:1889:    case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiController.cpp:2456:    case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/MidiController.cpp:2530:    case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/src/ParameterModulation.cpp:4093:MessageIn MessageIn::SceneBlendIncDec(std::uint64_t timestamp, float delta) {
HEAD:projects/synth/src/ParameterModulation.cpp:4096:    message.type = Type::SceneBlendIncDec;
HEAD:projects/synth/src/ParameterModulation.cpp:4229:    case MessageIn::Type::SceneBlendIncDec:
HEAD:projects/synth/tests/blocks_tests.cpp:156:        case MessageIn::Type::SceneBlendIncDec:
```

Seventeen lines. Each verdict:

- `MidiConfigBlocks.hpp:75` — the comment counting the kinds and naming the
  last ones. **Needed:** it becomes 29 kinds and names the two new last ones.
- `MidiConfigBlocks.cpp:133`, `MidiConfigViewModel.cpp:52`, `:96`, `:197`,
  `:793`, `MidiController.cpp:1889`, `:2456`, `:2530`,
  `ParameterModulation.cpp:4229`, `blocks_tests.cpp:156` — exhaustive switches
  over the kind. **Needed:** a case for each new kind, sorting and presenting
  as the scene-blend increment does, since neither is a row-dropdown kind
  either.
- `MidiController.cpp:238`, `:239`, `:297`, `:298` — the kind's JSON name in
  both directions. **Needed:** `"tempoBpmIncDec"` and
  `"setTempoBpmNormalized"`.
- `ParameterModulation.cpp:4093`, `:4096` — the factory. **Needed:** one
  factory each.
- `tests/viewmodel_tests.cpp`'s row-dropdown catalog table. **Not needed**, for
  the reason the scene-blend increment is not in it: the table lists kinds a
  system row's dropdown offers, and neither tempo kind is one. The grep above
  shows it holds no `SceneBlendIncDec` row to copy.

**The `shiftedJob` family, and whether a second value needs each site:**

```
$ git grep -in -E "shiftedjob|EncoderShiftedJob" HEAD -- projects/synth/include projects/synth/src | cut -c1-140
HEAD:projects/synth/include/synth/ControllersPageUI.hpp:2244:                    // EncoderTurnShiftedJobIndex() and EncoderShiftedJobCatalo
HEAD:projects/synth/include/synth/ControllersPageUI.hpp:2246:                    const auto& shiftedJobCatalog = EncoderShiftedJobCatalog();
HEAD:projects/synth/include/synth/ControllersPageUI.hpp:2247:                    for (int ix = 0; ix < static_cast<int>(shiftedJobCatalog.si
HEAD:projects/synth/include/synth/ControllersPageUI.hpp:2249:                        options.push_back({std::to_string(ix), shiftedJobCatalo
HEAD:projects/synth/include/synth/ControllersPageUI.hpp:2252:                                  vm.EncoderTurnShiftedJobIndex(controllerIx, s
HEAD:projects/synth/include/synth/MidiConfigViewModel.hpp:185:        // see EncoderShiftedJob). Shown only when the row dropdown offers
HEAD:projects/synth/include/synth/MidiConfigViewModel.hpp:349:// Display names for every EncoderShiftedJob, in declaration order
HEAD:projects/synth/include/synth/MidiConfigViewModel.hpp:354:// case and EncoderTurnShiftedJobIndex map a choice index to an
HEAD:projects/synth/include/synth/MidiConfigViewModel.hpp:355:// EncoderShiftedJob by declaration order, so the list stays in that order.
HEAD:projects/synth/include/synth/MidiConfigViewModel.hpp:356:const std::vector<std::string>& EncoderShiftedJobCatalog();
HEAD:projects/synth/include/synth/MidiConfigViewModel.hpp:566:    // (0 = none, 1 = Scene Blend -- see EncoderShiftedJob), so a JUCE combo
HEAD:projects/synth/include/synth/MidiConfigViewModel.hpp:571:    int EncoderTurnShiftedJobIndex(std::size_t controllerIx, MidiConfigSection
HEAD:projects/synth/include/synth/MidiController.hpp:229:enum class EncoderShiftedJob {
HEAD:projects/synth/include/synth/MidiController.hpp:238:    EncoderShiftedJob shiftedJob = EncoderShiftedJob::None;
HEAD:projects/synth/src/MidiConfigBlocks.cpp:1026:        if (mappings[ix].shiftedJob == EncoderShiftedJob::None) {
HEAD:projects/synth/src/MidiConfigBlocks.cpp:1030:                const bool continues = cur.shiftedJob == EncoderShiftedJob::None &&
HEAD:projects/synth/src/MidiConfigViewModel.cpp:373:const std::vector<std::string>& EncoderShiftedJobCatalog() {
HEAD:projects/synth/src/MidiConfigViewModel.cpp:374:    // All EncoderShiftedJob choices, in declaration order (MidiController.hpp):
HEAD:projects/synth/src/MidiConfigViewModel.cpp:2067:int MidiConfigViewModel::EncoderTurnShiftedJobIndex(std::size_t controllerIx, MidiConfi
HEAD:projects/synth/src/MidiConfigViewModel.cpp:2084:    return static_cast<int>(mapping->shiftedJob);
HEAD:projects/synth/src/MidiConfigViewModel.cpp:2783:                    if (!IsIntegerInRange(value, 0.0, static_cast<double>(EncoderShifte
HEAD:projects/synth/src/MidiConfigViewModel.cpp:2787:                    mapping->shiftedJob = static_cast<EncoderShiftedJob>(static_cast<in
HEAD:projects/synth/src/MidiController.cpp:729:            if (shift_ != nullptr && shift_->held && mapping->shiftedJob == EncoderShiftedJob
HEAD:projects/synth/src/MidiController.cpp:2231:    if (value.shiftedJob == EncoderShiftedJob::SceneBlend)
HEAD:projects/synth/src/MidiController.cpp:2232:        json.SetNew("shiftedJob", arena.String("sceneBlend"));
HEAD:projects/synth/src/MidiController.cpp:2246:    if (ObjectHasKey(json, "shiftedJob")) {
HEAD:projects/synth/src/MidiController.cpp:2247:        const JSON shiftedJob = json.Get("shiftedJob");
HEAD:projects/synth/src/MidiController.cpp:2248:        if (!IsString(shiftedJob) || std::string_view(shiftedJob.StringValue()) != "sceneBle
HEAD:projects/synth/src/MidiController.cpp:2251:        parsed.shiftedJob = EncoderShiftedJob::SceneBlend;
HEAD:projects/synth/src/MidiController.cpp:3644:            if (mapping.shiftedJob != EncoderShiftedJob::None) {
```

Verdicts:

- `MidiController.hpp:229` — the enum. **Needed:** `TempoBpm` appended.
- `MidiController.cpp:729` — the shifted dispatch. **Needed:** it tests one
  enumerator today and must handle both, with the tempo branch pushing the two
  new kinds.
- `MidiController.cpp:2231`, `:2232`, `:2248`, `:2251` — the JSON round trip,
  each written against the one value. **Needed:** written only when set, read
  back by name, anything else still refused.
- `MidiConfigViewModel.cpp:373` and its comment at `:374` — the display
  catalog. **Needed:** "BPM" appended, and the comment lists the choices.
- `MidiConfigViewModel.hpp:566` — the comment giving the index meanings.
  **Needed:** it names two choices by number and there will be three.
- `MidiConfigViewModel.cpp:2783`, `:2787`, `:2067`, `:2084`,
  `ControllersPageUI.hpp:2244` to `:2252` — validation against the catalog's
  size, the index cast, the read-back and the combo's option loop. **Not
  needed:** each is written over the whole catalog or over the enumerator's
  ordinal, so a third choice flows through unchanged. This is what makes the
  page change one catalog entry.
- `MidiConfigBlocks.cpp:1026`, `:1030` — block reconstruction. **Not needed:**
  both compare against `None`, so a tempo-shifted turn stays out of a block
  exactly as a blend-shifted one does. A check covers it.
- `MidiController.cpp:3644` — profile validation rejecting a shifted push.
  **Not needed:** it compares against `None`.
- `MidiConfigViewModel.hpp:184` (matched by this grep at the adjacent line
  `:185`, "see EncoderShiftedJob") — the `Field::ShiftAction` enumerator's
  comment. **Needed:** it names the choices by number too ("two-entry
  catalog (0 = none, 1 = Scene Blend"), the same claim as `:566` above.
- `MidiConfigViewModel.hpp:349`, `:354`, `:355`, `:356`, `:571`,
  `MidiController.hpp:238` — declarations and comments phrased over "every
  choice" rather than over the one. **Not needed.**
- `ControllersPageUI.hpp`'s encoder-turn branch of the `Field::ShiftAction`
  combo builder (the `section == MidiConfigSection::Encoders` branch that
  calls `EncoderTurnShiftedJobIndex()` and `EncoderShiftedJobCatalog()`)
  carries its own count-naming comment ("two-entry catalog (none, Scene
  Blend)"), which names neither `shiftedjob` nor `EncoderShiftedJob` and so
  does not appear in this grep's output at all. **Needed**, for the same
  reason as `:566` and `:184` above — found only by grepping the count's own
  words ("two-entry") rather than the symbol.

## Capabilities

### Modified Capabilities
- `synth-midi-instrument`: smi-13 and smi-17 modified.
- `synth-runtime-ui`: sru-66 modified.

## Impact

- `include/synth/ParameterModulation.hpp`, `include/synth/MasterClock.hpp`
  (unchanged; named because the bus now depends on it),
  `include/synth/MidiController.hpp`, `include/synth/MidiAppCatalog.hpp`,
  `include/synth/MidiConfigViewModel.hpp`,
  `include/synth/MidiConfigBlocks.hpp` (the comment counting the kinds),
  `include/synth/ControllersPageUI.hpp` (the shifted-job combo's own
  count-naming comment),
  `include/synth/Engine.hpp` (the bus wiring),
  `src/ParameterModulation.cpp`, `src/MidiController.cpp`,
  `src/MidiConfigViewModel.cpp`, `src/MidiConfigBlocks.cpp`.
- Tests: `tests/instrument_tests.cpp`, `tests/parameter_modulation_tests.cpp`,
  `tests/viewmodel_tests.cpp`, `tests/blocks_tests.cpp`,
  `tests/controllers_page_ui_tests.cpp`, `tests/engine_tests.cpp`.

## Delivery

Every addition is additive at the boundary. A new enumerator appended after
the last one moves no ordinal; a new optional JSON key reads as absent on
every document written before it; a bus with no clock drops the two new kinds,
so an app that names no tempo action sees no change at all. No existing
message, mapping or saved document changes meaning. The one non-additive edit
is a spec example and its test swapping `"tempo"` for `"swing"` as the
unknown shifted job, which changes no behaviour.

This work is done on the fork on the `shifted-encoder-turns` branch, beside
the changes already active there, and is delivered as the next pull request
from the fork against upstream `main` after the open ones. The pull request
description carries step-by-step testing instructions for Shift + a knob on a
relative controller and on an absolute one.
