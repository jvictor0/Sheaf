# Delta — `synth-app-runtime`

The standalone opens one `juce::MidiInput` per controller slot and every slot
pushes into the same MIDI bus, which is written for one producer. Whether JUCE
calls two ports' callbacks on two threads could not be settled by reading; an
operator run with two controllers settles it.

## ADDED Requirements

### Requirement: sar-35 — MIDI input: two ports never lose or duplicate a message
WHEN two or more MIDI input ports deliver messages at the same time, THE runtime SHALL place every message on the engine's MIDI bus exactly once, in the order each port delivered it, whichever threads the host calls the ports' callbacks on.

#### Scenario: Two controllers stream at once
- **WHEN** a MIDI Fighter Twister and an APC40 mkII both stream controller messages into the standalone
- **THEN** every message from each device reaches the bus once
- Check: operator step: the thread-id run with both devices in the frogg3rs change `frogg3rs-operator-runs`; a second thread id means this change's fix task applies
