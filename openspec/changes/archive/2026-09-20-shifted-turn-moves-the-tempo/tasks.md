# Tasks — `shifted-turn-moves-the-tempo`

Every new test is shown to fail with its production change reverted, by the
executor, before the task is reported done, and the report says so. Builds run
under `nice`, `-j2`, one at a time.

- [x] 1. Give the message bus the clock and the range: NEW
      `MessageInBus::SetTempoClock(MasterClock*, float minimumBpm, float
      maximumBpm)` beside `SetGridManager` and `SetAppActionOut`, with a
      forward declaration of `MasterClock` in `ParameterModulation.hpp` and
      the include in `ParameterModulation.cpp`. A bus with no clock or no
      range applies no tempo message.
      Check: NEW `TempoMessagesAreInertWithoutAClockRangeOrInternalTempo` in
      `parameter_modulation_tests.cpp` passes: a tempo increment applied on a
      bus given no clock, on a bus given no range, and on a bus whose clock
      is slaved to external MIDI leaves the tempo unchanged in all three. The
      same case adds a positive control ahead of the three inert ones: a bus
      given a clock, a range, and no external slaving moves the tempo, so the
      three inert cases cannot be mistaken for a bus that drops every tempo
      message regardless of its state.
- [x] 2. Add the two tempo message kinds: NEW
      `MessageIn::Type::TempoBpmIncDec` and NEW
      `MessageIn::Type::SetTempoBpmNormalized`, appended after
      `SceneBlendIncDec` so no existing ordinal moves, their factories
      `MessageIn::TempoBpmIncDec(timestamp, deltaBpm)` and
      `MessageIn::SetTempoBpmNormalized(timestamp, normalized)`, their
      `MessageInBus::Apply` cases (the increment adds the step to
      `MasterClock::TempoBpm()` and clamps to the range; the set places the
      normalized value across the range; both go through
      `MasterClock::SetTempoBpm`), their JSON names `"tempoBpmIncDec"` and
      `"setTempoBpmNormalized"` in both directions, and a case at every site
      in the proposal's `SceneBlendIncDec` family, including the kind count
      and last-kind names in `MidiConfigBlocks.hpp`'s `SystemMessageSortKey`
      comment.
      Check: NEW `TempoIncrementClampsAtBothEndsOfTheRange` in
      `parameter_modulation_tests.cpp` passes: on a bus whose range is 30 to
      300, from 31 the increments -5, -5, +5 give 30, 30, 35, and from 299
      the increments +5, -5 give 300 and 295. The proposal's Evidence lists
      each family site as changed or not needed, with the reason for the one
      not needed.
- [x] 3. Declare NEW `kTempoBpmPerEncoderDetent`, the tempo step one encoder
      detent is worth, in `include/synth/MidiController.hpp` beside
      `EncoderMidiInConfig::turnStep`, with a comment saying what it is in
      BPM: 1.0 BPM per detent, linear. Tempo is named by musicians in whole
      numbers, so a linear law lands a detent on an integer where a
      logarithmic one would land on a fraction the one-decimal readout would
      show; 1.0 also matches the on-screen BPM slider's own step, already
      what JUCE's `setRange` interval and the browser input's step use, so
      the knob and the slider agree on the finest move either can make.
      Check: `kTempoBpmPerEncoderDetent` reads `1.0` and task 4's
      `ShiftHeldTurnPushesTempoIncrementAndReleaseRestoresTheParameter`
      passes with it.
- [x] 4. Add the shifted job and its dispatch: NEW `EncoderShiftedJob::TempoBpm`
      appended after `SceneBlend`, serialized as `"shiftedJob": "tempoBpm"`
      written only when set and read back by that name, with a missing key
      still read as none and any other value still failing the load; NEW
      private `EncoderMidiInProcessor::DecodeTicks` returning the signed
      detent count, with `DecodeDelta` written as that count times
      `turnStep`; and the shifted branch in `EncoderMidiInProcessor::Process`
      handling Tempo beside Scene blend, a relative turn pushing
      `TempoBpmIncDec` with the detent count times
      `kTempoBpmPerEncoderDetent` and an Absolute turn pushing
      `SetTempoBpmNormalized` with the turn's normalized value.
      Check: NEW `ShiftHeldTurnPushesTempoIncrementAndReleaseRestoresTheParameter`
      and NEW `ShiftHeldAbsoluteTurnSetsTheTempoAcrossTheRange` in
      `instrument_tests.cpp` pass: the relative turn's first detent under
      Shift pushes a tempo increment equal to one detent's step and no
      parameter message, and after Shift releases the same turn pushes the
      parameter message; the Absolute turn at value 127 under Shift leaves a
      clock with range 30 to 300 reading 300 and pushes no parameter message.
      The existing `EncoderTurnJsonRoundTripsShiftedJobAndRejectsAnUnknownOne`
      passes with a Tempo turn round-tripped alongside the Scene blend one.
      The relative assertion is written against
      `kTempoBpmPerEncoderDetent` itself, not against a hardcoded figure, so
      it tracks the constant rather than duplicating its value.
- [x] 5. Bind the range to the app's own declaration: NEW
      `MidiAppCatalog::tempoAction`, a bare action name matched with an empty
      value in the shape `encoderPressAction` has; `Engine` resolves it once
      through the existing `FindMidiAppAction` where it reads the catalog and
      calls `SetTempoClock` on both `uiBus_` and `midiBus_` where it calls
      `SetGridManager` and `SetAppActionOut`, passing the resolved action's
      analog range. A catalog naming no tempo action, one that does not
      resolve, or one with no analog range leaves both buses without a clock.
      Check: NEW `engine_tempo_action_range_reaches_the_message_bus` in
      `engine_tests.cpp` passes: an engine built from a catalog naming a
      tempo action with an analog range moves its master clock's tempo when a
      tempo increment is pushed onto the MIDI bus, and an engine built from a
      catalog naming none does not.
- [x] 6. Controllers page: append "BPM" to `EncoderShiftedJobCatalog()` after
      "Scene Blend" and correct the comment above it and the index comment on
      `MidiConfigViewModel::EncoderTurnShiftedJobIndex`, both of which name
      the choices by number. The same count is named in two more comments
      that a grep for `EncoderShiftedJob` does not reach, because neither
      line contains that name: the `Field::ShiftAction` enumerator's comment
      in `MidiConfigViewModel.hpp` ("row's own fixed two-entry catalog (0 =
      none, 1 = Scene Blend") and the comment on the encoder-turn branch of
      `Field::ShiftAction` handling in `ControllersPageUI.hpp` ("two-entry
      catalog (none, Scene Blend)"); correct both the same way.
      Check: `grep -rn "two-entry" include/synth/MidiConfigViewModel.hpp
      include/synth/ControllersPageUI.hpp` prints nothing. NEW
      `TestTurnRowShiftComboOffersBpmAndCommits` in
      `controllers_page_ui_tests.cpp` passes: a turn row's Shift combo offers
      none, Scene Blend and BPM in that order, commits BPM, and shows the
      committed value after a rebuild, with the turn's mapping carrying
      shifted job Tempo. The existing
      `TurnShiftFieldEditCommitsSceneBlendAndNoneClearsIt`,
      `TurnRowsExposeShiftFieldOnlyWhenShiftIsOffered` and
      `ReconstructEncoderBlocksKeepsAShiftedTurnOutOfItsBlock` pass unchanged.
      The existing `TestTurnRowShiftComboOffersSceneBlendAndCommits` is
      updated for the combo's new third entry: its option-count assertion
      moves from 2 to 3 and it gains an assertion that option 2 reads "BPM".
- [x] 7. In `instrument_tests.cpp`'s
      `EncoderTurnJsonRoundTripsShiftedJobAndRejectsAnUnknownOne`, use
      `"swing"` as the unknown shifted job in place of `"tempo"`, which is no
      longer a plain example of one now that `"tempoBpm"` is known.
      Check: the test names `"swing"` and no longer names `"tempo"`, and
      passes; a document naming `"tempo"` still fails to load.
- [x] 8. Run Sheaf's full `projects/synth` suite by running every test binary
      by path after `make test` stops, and build and run the miniapp runtime
      target, which that suite does not build.
      Check: pass and fail counts reported per binary as measured; every
      failure is either fixed here or shown to fail identically at
      `40aea92d`. A test named red in this report is reported, never edited.
