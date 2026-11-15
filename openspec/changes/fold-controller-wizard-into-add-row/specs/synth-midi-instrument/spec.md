# Delta — `synth-midi-instrument`

## MODIFIED Requirements

### Requirement: smi-8 — Live edits: config changes rebuild processors
WHEN the instrument configuration is edited through the configuration UI, THE message thread SHALL apply the committed edit to the engine's live instrument configuration in a way that cannot race the audio thread's patch-message application (the sar-7 block-boundary patch drain remains the only audio-side writer, and UI-edit application SHALL be serialized against it), SHALL rebuild the affected controller's MIDI processor slot and reconcile connections through the existing shared path, SHALL construct Active processors from Active profiles, and SHALL construct only an explicit drop/no-op input processor with no terminal realtime processor, output processors, thru processors, or sender sink for a Blacklisted slot; WHEN a patch or runtime-configuration load changes the instrument configuration, the existing patch/configuration message flow SHALL apply it followed by the same message-thread rebuild and reconciliation, so UI edits and loads converge on one rebuild path; deleting an Active slot SHALL close affected endpoints, while generating a slot as Active SHALL make its processors and endpoints live without restart.

#### Scenario: Mapping edit takes effect
- **WHEN** the user changes an Active encoder mapping's target slot position and commits
- **THEN** the next matching hardware CC drives the newly mapped position

#### Scenario: UI edits and configuration loads share the rebuild path
- **WHEN** an instrument change arrives from the configuration UI and another from a patch or runtime-configuration load
- **THEN** both trigger the same processor rebuild and reconciliation path

#### Scenario: Edits do not race the audio thread
- **WHEN** a UI instrument edit commits while a patch-load message is pending on the patch input bus
- **THEN** the live instrument configuration observes serialized application with no concurrent mutation

#### Scenario: Added active controller becomes live
- **WHEN** the add row installs an Active controller with generated profile and present devices
- **THEN** reconciliation connects it and its processors are active without restart

#### Scenario: Blacklisted processor chain drops everything
- **WHEN** a Blacklisted slot's explicit drop input processor receives an ordinary, SysEx, or realtime MIDI message during or after a rebuild window
- **THEN** it emits no parameter, grid, clock, or transport message
- **AND** the slot has no terminal realtime, thru, output, or sender-sink route

Dropped scenario: **Reconfigure activates a blacklisted record**. Configure, the released row's path back to Active, is removed (`synth-runtime-ui` sru-4, D4); nothing reactivates a Blacklisted slot any more, so this scenario has no code left to cover.
