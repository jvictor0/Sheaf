# Delta — `synth-midi-instrument`

The output processors ignored whether the shared MIDI sender accepted a
message and marked the value sent either way. The MF Twister processor sent
each LED update with three `Enqueue` calls and then marked its cache valid;
the shared position path did the same in every mode except Absolute, and the
Twister ran Signed7Bit; the WRLD.Bldr colour path, the system-message CC and
colour processors, the Launchpad grid processor, the Generic encoder
processor and the connect-time SysEx processor each recorded the value or
cleared the pending message before knowing whether it was accepted. A
declined update was therefore never sent again until the value changed, or,
for the connect-time SysEx, until the next connect. A capacity-1 sender run
confirmed it for the Twister: one CC arrived and three never did.

## ADDED Requirements

### Requirement: smi-18 — Output feedback: a declined update is sent later
WHEN the MIDI sender declines an output processor's message because its queue is full, THE output processor SHALL keep that message pending and send it on a later `Process()` pass once the queue accepts it, SHALL enqueue nothing more in the pass in which a message was declined, and SHALL mark a value as sent only after every message for that value was accepted; WHEN the queue never fills, THE output processor SHALL send exactly the messages it sends today, in the same order. This holds for every output processor: the Generic and Twister encoder processors and their shared position path, the WRLD.Bldr encoder processor's colours, the system-message CC processor, the WRLD.Bldr system processor, the Launchpad grid processor and the connect-time SysEx processor.

#### Scenario: A full queue delays the LED rings, then they follow
- **WHEN** a Twister output processor with one mapping runs against a sender of capacity 1, and the sender is drained between `Process()` passes with the cell unchanged
- **THEN** every colour, brightness and ring message for that cell arrives exactly once across the passes, one per pass while the queue holds one
- **AND** no pass enqueues a message after the sender has declined one in that pass
- Check: `tests/parameter_modulation_tests.cpp: twister_output_resends_a_declined_value_on_the_next_pass_and_nothing_jumps_it`

#### Scenario: Every other output processor catches up the same way
- **WHEN** each of the WRLD.Bldr encoder, system-message CC, WRLD.Bldr system, Launchpad grid and connect-time SysEx processors runs with two messages to send against a sender of capacity 1, drained between passes
- **THEN** both messages arrive exactly once, in order, one per pass
- Check: `tests/parameter_modulation_tests.cpp: the_other_five_output_processors_resend_a_declined_message_on_the_next_pass`

#### Scenario: With room in the queue the bytes do not change
- **WHEN** each output processor runs against a sender with its default capacity
- **THEN** the bytes sent are the bytes the unchanged code sends, message for message
- Check: `tests/parameter_modulation_tests.cpp: twister_output_pins_bytes_through_a_value_change_and_a_disconnect`, `the_other_five_output_processors_do_not_resend_an_unchanged_value`
