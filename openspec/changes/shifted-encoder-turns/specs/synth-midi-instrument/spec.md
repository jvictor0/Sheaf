# Delta — `synth-midi-instrument`

smi-16 is modified as it stands in the active change `shift-and-file-export`
(jvictor0/Sheaf#14), which added it. Its body changes one clause, "consumed
only by that same profile's system-button processor", to "read only by that
same profile's system-button and encoder processors", and gains the sentence
pointing to smi-17; every scenario is carried forward word for word.

## MODIFIED Requirements

### Requirement: smi-16 — Shift: a held button swaps other buttons' presses
WHEN a system-button mapping's press targets `Shift`, THE synth system SHALL hold per-profile state, a held flag, set by that button's press and cleared by its release, read only by that same profile's system-button and encoder processors and never placed on the message bus; neither edge SHALL push any message. WHILE held is set, THE synth system SHALL, for any other system-button press on that profile whose association carries a shifted press, push that shifted press in place of the ordinary press, stamped as the ordinary press would have been, and SHALL push the ordinary press for an association without one; a button's release SHALL behave exactly as it does unshifted. What a held Shift does to an encoder turn is smi-17. Shift and Hold Drill SHALL be independent: either may be held while the other is. A Shift press whose release never arrives SHALL leave the profile shifted until the next Shift press and release.

#### Scenario: Shift swaps a press and its release restores it
- **WHEN** the Shift button is held, a button with a shifted press is pressed, Shift is released, and the same button is pressed again
- **THEN** the first press pushes the shifted message and the second pushes the ordinary one
- **AND** no message is pushed for the Shift button's own press or release
- Check: `instrument_tests.cpp: ShiftHeldSwapsPressForShiftedPressAndReleaseClearsIt`

#### Scenario: A button without a shifted press ignores Shift
- **WHEN** the Shift button is held and a button with no shifted press is pressed
- **THEN** its ordinary press is pushed
- Check: `instrument_tests.cpp: ShiftHeldSwapsPressForShiftedPressAndReleaseClearsIt`

#### Scenario: Shift and Hold Drill do not interfere
- **WHEN** both a Shift button and a Hold Drill button are held and an encoder is turned and a shifted button pressed
- **THEN** the turn drills once and the press pushes the shifted message
- Check: `instrument_tests.cpp: ShiftAndHoldDrillAreIndependent`

## ADDED Requirements

### Requirement: smi-17 — Shift: an encoder turn's shifted job
WHEN an encoder turn mapping carries a shifted job and that profile's Shift is held, THE synth system SHALL push the shifted job in place of the turn's parameter message, and SHALL push the parameter message exactly as before while Shift is not held or the mapping carries no shifted job. The shifted jobs SHALL be Scene blend and Tempo. A shifted job SHALL address state belonging to the whole instrument, never the state of the parameter bank the turn's own parameter currently belongs to, and pushing it SHALL consult neither the turn's slot nor its parameter bank, so the same turn under Shift SHALL move the shifted job by the same amount whichever parameter bank is selected for its own parameter. For Scene blend: in a relative encoder mode a turn SHALL push a scene-blend increment carrying the turn's decoded delta, which the message bus SHALL apply on the audio thread by adding it to the current blend and clamping the result to 0..1; in Absolute mode a turn SHALL push a scene-blend set to the turn's normalized value. For Tempo: in a relative encoder mode a turn SHALL push a tempo increment carrying the turn's detent count times the library's tempo step per detent, and in Absolute mode a turn SHALL push a normalized tempo set carrying the turn's normalized value; the message bus SHALL apply both on the audio thread against the master clock, the increment by adding the step to the clock's current tempo and the set by placing the normalized value across the range, each clamped to the tempo range the app's catalog supplies (smi-13), and SHALL apply neither when it has no clock or no range. A tempo move SHALL go through the master clock's own tempo setter, so a clock slaved to external MIDI SHALL be left alone. WHILE Hold Drill is held, a turn SHALL drill exactly as it does without Shift, whether or not Shift is also held. After Hold Drill is released while Shift is still held, a turn with a shifted job SHALL do the shifted job. An encoder push mapping SHALL carry no shifted job, and a profile config whose push mapping carries one SHALL be reported invalid. A turn's shifted job SHALL serialize with its mapping under a `shiftedJob` key written only when the mapping has one; a document without the key SHALL load with no shifted job, and a document naming a shifted job the library does not know SHALL fail to load with the target configuration unchanged. The scene-blend increment and the two tempo kinds SHALL each be a new message kind appended after the last one, so no existing kind's ordinal moves.

#### Scenario: A shifted turn moves the scene blend and releasing Shift restores the knob
- **WHEN** a relative turn mapping with shifted job Scene blend is turned while Shift is held, Shift is released, and it is turned again
- **THEN** the first turn pushes a scene-blend increment carrying the decoded delta and no parameter message
- **AND** the second turn pushes the parameter message and no scene-blend message
- Check: `instrument_tests.cpp: ShiftHeldTurnPushesSceneBlendIncrementAndReleaseRestoresTheParameter`

#### Scenario: An absolute shifted turn sets the blend
- **WHEN** an Absolute-mode turn mapping with shifted job Scene blend receives value 127 while Shift is held
- **THEN** a scene-blend set of 1.0 is pushed and no parameter message
- Check: `instrument_tests.cpp: ShiftHeldAbsoluteTurnSetsTheSceneBlend`

#### Scenario: Hold Drill still drills a knob that has a shifted job
- **WHEN** Shift and Hold Drill are both held and a turn mapping with shifted job Scene blend is turned twice
- **THEN** exactly one push message is pushed for that knob and no scene-blend message
- Check: `instrument_tests.cpp: HoldDrillDrillsAShiftedTurnWhileBothAreHeld`

#### Scenario: The increment adds to the blend and clamps
- **WHEN** the blend is 0.95 and increments of +0.1 and then -2.0 are applied
- **THEN** the blend reads 1.0 after the first and 0.0 after the second
- Check: `parameter_modulation_tests.cpp: SceneBlendIncrementAddsToTheBlendAndClamps`

#### Scenario: A shifted job round-trips, its absence reads as none, and an unknown one fails
- **WHEN** an encoder config whose turn carries shifted job Scene blend is serialized and reloaded
- **THEN** that turn's shifted job is Scene blend and every other turn has none
- **AND** the serialized form of a turn without a shifted job has no `shiftedJob` key
- **WHEN** a document names `shiftedJob` `"swing"`
- **THEN** the load fails and the target configuration is unchanged
- Check: `instrument_tests.cpp: EncoderTurnJsonRoundTripsShiftedJobAndRejectsAnUnknownOne`

#### Scenario: A push cannot carry a shifted job
- **WHEN** a profile config's encoder push mapping carries shifted job Scene blend
- **THEN** the config is reported invalid and cannot be committed
- Check: `instrument_tests.cpp: ProfileWithAShiftedEncoderPushIsInvalid`

#### Scenario: A tempo-shifted turn moves the tempo and releasing Shift restores the knob
- **WHEN** a relative turn mapping with shifted job Tempo is turned one detent clockwise while Shift is held, Shift is released, and it is turned again
- **THEN** the first turn pushes a tempo increment whose step is one detent's worth and no parameter message
- **AND** the second turn pushes the parameter message and no tempo message
- Check: `instrument_tests.cpp: ShiftHeldTurnPushesTempoIncrementAndReleaseRestoresTheParameter`

#### Scenario: An absolute tempo-shifted turn places the tempo across the range
- **WHEN** an Absolute-mode turn mapping with shifted job Tempo receives value 127 while Shift is held, on a bus whose tempo range is 30 to 300
- **THEN** the master clock's tempo reads 300 and no parameter message is pushed
- Check: `instrument_tests.cpp: ShiftHeldAbsoluteTurnSetsTheTempoAcrossTheRange`

#### Scenario: A tempo increment clamps at both ends and does not wrap
- **WHEN** the tempo is 31 on a bus whose range is 30 to 300 and increments of -5, then -5 again, then +5 are applied
- **THEN** the tempo reads 30, then 30, then 35
- **WHEN** the tempo is 299 and increments of +5 and then -5 are applied
- **THEN** the tempo reads 300 and then 295
- Check: `parameter_modulation_tests.cpp: TempoIncrementClampsAtBothEndsOfTheRange`

#### Scenario: A tempo message does nothing without a clock, a range, or an internal clock
- **WHEN** a tempo increment is applied on a bus given no clock, on a bus given no range, and on a bus whose clock is slaved to external MIDI
- **THEN** no tempo changes in any of the three
- Check: `parameter_modulation_tests.cpp: TempoMessagesAreInertWithoutAClockRangeOrInternalTempo`
