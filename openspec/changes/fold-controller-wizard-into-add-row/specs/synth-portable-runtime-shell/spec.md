# Delta — `synth-portable-runtime-shell`

## MODIFIED Requirements

### Requirement: sprs-3 — Services: compile-time-swappable host adapters
WHEN the shared component needs audio, MIDI/controller, patch/file, deadline, or runtime-configuration behavior, THE component SHALL obtain it through a host services concept selected at compile time, with JUCE and browser adapters that expose equivalent generic page snapshots and actions while keeping host API objects outside application code.

#### Scenario: JUCE services retain desktop behavior
- **WHEN** the JUCE main component refreshes or dispatches a runtime page
- **THEN** its adapter delegates to the existing JUCE runtime, engine, audio manager, MIDI connection manager, and patch operations
- **AND** existing desktop page behavior remains available

#### Scenario: Browser audio choices remain generic
- **WHEN** the browser Audio page snapshot is built
- **THEN** it contains exactly one output option with id `system_default` and label `System Default`
- **AND** it contains no input selector or named output-device enumeration

#### Scenario: Browser runtime pages remain application-agnostic
- **WHEN** browser sidebar actions open Audio, Controllers, or File
- **THEN** the shared runtime page models build and dispatch those pages through browser services
- **AND** application code and browser HTML remain unchanged for the selected page

#### Scenario: Controllers refresh through host services
- **WHEN** a UI tick observes changed controller endpoints, connection state, or instrument state
- **THEN** the active host services refresh the shared Controllers page surface and its device list
- **AND** JUCE and browser hosts do not maintain separate controller page models

#### Scenario: File page patch semantics are shared
- **WHEN** the shared File page dispatches direct or confirmed patch actions for New, Save, Confirmed Save As, Confirmed Overwrite Save As, or Confirmed Load
- **THEN** one JUCE-free runtime-file helper maps those actions to host-provided patch operations and shared status text for both JUCE and browser services
- **AND** raw Save As and Load actions remain shared File page surface actions that open the file browser/confirmation flow rather than invoking patch operations directly
- **AND** the helper projects current patch directory, patch name, patches root, and status text into `FilePageSnapshot` for both hosts
- **AND** host adapters provide only the backend-specific patch operation bindings and data paths, with JUCE patch bindings continuing to log their patch command results
- **AND** neither host maintains an independent File action ladder with duplicated status strings

#### Scenario: Browser deadline sample comes from the runtime-owned callback
- **WHEN** the browser host refreshes the sidebar before a browser callback-load metric has been published
- **THEN** its services adapter supplies 0.0 percent
- **AND** after runtime-owned AudioWorklet callbacks have run, it supplies the averaged callback deadline percentage rather than deriving a misleading value from the legacy audio render timer
- **AND** the JUCE host continues to supply its device-manager CPU usage percentage

No scenario is dropped. The "File page patch semantics are shared" scenario keeps every assertion; only its action list loses Revert (`synth-runtime-ui` D8).
