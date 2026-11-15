# Delta — `synth-app-runtime`

## ADDED Requirements

### Requirement: sar-43 — Audio: input opens only with record permission
WHEN the JUCE runtime is about to open an audio input device, whether chosen on the Audio page or reapplied from a persisted selection at start, THE runtime SHALL first request the platform's record permission through `juce::RuntimePermissions`, SHALL open the input only when it is granted, and when it is refused SHALL keep the output device running and publish an Audio page status saying that microphone access was not granted.

#### Scenario: Desktop platforms are unchanged
- **WHEN** an input is chosen on macOS or Windows
- **THEN** the permission callback grants at once and the input opens as before
- Check: `juce/RuntimeShellSessionTests.cpp` input checks, run through `apps/miniapp`'s `test` target.

#### Scenario: A refused permission keeps the output
- **WHEN** an input is chosen on Android and the record permission is refused
- **THEN** the output keeps running with no input opened
- **AND** the Audio page status says microphone access was not granted
- Check: the frogg3rs Android emulator check (`frogg3rs-android-app` task 4.9).
