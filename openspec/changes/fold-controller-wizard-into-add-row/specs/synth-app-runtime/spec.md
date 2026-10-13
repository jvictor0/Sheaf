# Delta — `synth-app-runtime`

## MODIFIED Requirements

### Requirement: sar-3 — Context: application access to managers and configuration
WHEN the runtime initializes an application, THE runtime SHALL pass a pointer to an `AppContext` holding non-owning, address-stable pointers to the parameter manager, runtime-owned grid manager, runtime-owned master clock, patch manager, UI message input bus, MIDI message input bus, parameter message output bus, patch message input and output buses, MIDI sender, live and default MIDI instrument configurations, the runtime configuration, and the host's shared monotonic timestamp provider (so application UI code timestamps messages from the same clock as the engine); the grid-manager pointer SHALL be available during `Init` so an application can declare fixed grid/slot topology before runtime finalization, the master-clock pointer SHALL be available during `Init` for stable storage but SHALL report prepared output-sample-domain values and a current committed plan only after the host prepare entry point and first block commit respectively, while the context's UI-state pointer SHALL be null during `Init` and SHALL be populated before MIDI processors, audio, or UI processing begin; all pointees SHALL remain valid for the application's lifetime.

#### Scenario: Context grants manager access during Init
- **WHEN** the application's `Init(AppContext*)` runs
- **THEN** the application can create groups, register modules and parameters, configure pages and banks through the context's parameter manager pointer, declare fixed grid/slot topology through the runtime-owned grid-manager pointer, and retain the runtime-owned master-clock pointer

#### Scenario: Context pointers remain stable
- **WHEN** the application stores the context pointer during `Init` and dereferences a member from a hook permitted to touch that member under the sar-7 threading contract
- **THEN** every pointer refers to the same live object the runtime constructed
- **AND** the context documentation names the thread role permitted to use each member

#### Scenario: UI state populated after topology lock
- **WHEN** application initialization completes and the runtime creates the manager UI state
- **THEN** the context's UI-state pointer is set before the first MIDI processor rebuild, audio callback, or UI frame

#### Scenario: Application seeds a default instrument
- **WHEN** the application's `Init` populates the live MIDI instrument configuration through the context
- **THEN** the runtime snapshots it as the default instrument restored by new

#### Scenario: Clock is prepared from negotiated audio
- **WHEN** the host calls Engine prepare with its actual sample rate and block size
- **THEN** the retained master clock computes its increment and scheduling horizon from those negotiated values before the first application block

### Requirement: sar-8 — Patches: runtime startup load and command orchestration
WHEN the runtime starts and the runtime configuration records a patch version file that still exists, THE runtime SHALL open that version, restoring its parameter values only, and SHALL leave the MIDI instrument configuration the runtime configuration already restored untouched, even under an application catalog that sets `patchCarriesMappings` (spp-4). WHEN there is no such record, THE runtime SHALL instead load the most recent patch under the runtime-owned patches root, selected deterministically by the library's sortable version-file naming — the patch directory containing the lexicographically greatest version filename, ties broken by directory name — applying that patch's instrument too under `patchCarriesMappings`, and SHALL then record that version and save the runtime configuration. WHEN the recorded file no longer exists, THE runtime SHALL fall back to the same newest-version selection, restoring parameters only, and SHALL record that version. WHEN the record is explicitly empty because New was the last action before this launch, THE runtime SHALL open no patch. THE runtime SHALL fall back silently to the application's initialized defaults when no patch exists. WHILE running, THE runtime SHALL expose new/save/save-as/load patch commands through the patch manager, write and read version files only through library persistence helpers; opening a patch from the File page applies that patch's instrument under `patchCarriesMappings` and then saves the runtime configuration, and Load, Save, Save As, and Save As (overwrite) each record their patch version and save the runtime configuration.

#### Scenario: Startup reopens the recorded patch version and keeps the runtime configuration's instrument
- **WHEN** the runtime starts and the runtime configuration records a patch version file that still exists
- **THEN** that version's parameter values are applied before audio starts
- **AND** the MIDI instrument configuration stays the one the runtime configuration restored, even under an application catalog that sets `patchCarriesMappings`
- Check: `engine_tests.cpp: engine_startup_restores_recorded_patch_parameters_and_the_runtime_configurations_instrument`

#### Scenario: Startup reopens the version last opened even when a newer one exists
- **WHEN** the runtime configuration records a version that is not the newest under the patches root
- **THEN** the recorded version is opened at the next launch, not the newest one
- Check: `engine_tests.cpp: engine_relaunch_reopens_the_patch_version_last_opened_even_when_a_newer_version_exists`

#### Scenario: Startup selection is deterministic under ties
- **WHEN** there is no recorded version, and two patch directories contain version files with identical names
- **THEN** the directory with the lexicographically greater name is selected

#### Scenario: Missing startup patch keeps defaults
- **WHEN** the runtime starts with an empty patches root
- **THEN** the application's `Init` defaults remain in effect and no persistence failure is reported

#### Scenario: Opening a patch from the File page applies its instrument and saves the runtime configuration
- **WHEN** a patch is opened under an application catalog that sets `patchCarriesMappings`
- **THEN** the patch's instrument replaces the live one on the next message-thread tick
- **AND** the runtime configuration is saved with that instrument, so a later launch keeps it
- Check: `engine_tests.cpp: engine_opening_a_patch_that_carries_mappings_saves_its_instrument_to_the_runtime_configuration`

#### Scenario: A configuration with no record opens the newest version, including its instrument, then starts recording
- **WHEN** the runtime starts with a runtime configuration written before this record existed
- **THEN** the newest saved version under the patches root is opened, applying its instrument under `patchCarriesMappings`
- **AND** that version is recorded and the configuration saved, so later launches follow the recorded-version rules, keeping edits made after that launch
- Check: `engine_tests.cpp: engine_relaunch_after_upgrading_records_the_opened_version_and_keeps_a_later_edit`

A recorded file that no longer exists falls back to the same newest-version selection, restoring parameters only, and records that version; no test covers this case. After New, the record is explicitly empty and no patch opens at the next launch; no test covers this case either.

### Requirement: sar-13 — Test rig: headless SynthRig harness
WHEN system-level tests need to drive an assembled application, THE repository SHALL provide a JUCE-free `SynthRig` test harness under `projects/synth/tests/support` that wraps the engine with an application core and provides: deterministic time driving (`RunBlocks`, `RunSamples`, `RunSeconds`) that pumps audio blocks with block-derived timestamps and runs the message-side tick each block on the single test thread; message injection through production paths (encoder turn/press/shift, gesture, scene, and blend messages onto the UI bus, and raw MIDI messages through the engine's MIDI input processor chain into the MIDI bus); observation of manager UI state, parameter values, and captured output with sticky NaN/Inf and peak invariants over every output sample; and patch save/load helpers that issue patch manager commands and pump for a bounded number of blocks, returning a status that distinguishes success, command failure, and timeout rather than hanging.

#### Scenario: Rig drives production engine code
- **WHEN** a rig test runs blocks
- **THEN** the same engine pump the JUCE runtime uses processes the buses, control-rate computation, and the application block hook

#### Scenario: Injected MIDI exercises real routing
- **WHEN** a rig test sends a raw MIDI message matching the active controller profile
- **THEN** the message flows through the MIDI input processor chain and MIDI bus into the manager
- **AND** the affected parameter's readback reflects the mapped change after settling

#### Scenario: Output invariants are sticky
- **WHEN** any processed sample contains NaN or Inf
- **THEN** the rig's NaN flag reports true for the remainder of the test until explicitly cleared

#### Scenario: Rig runs are deterministic
- **WHEN** the same rig test executes twice with the same seeds
- **THEN** injected message timestamps, block boundaries, and observed state sequences are identical

#### Scenario: Patch helpers cannot hang
- **WHEN** a rig patch helper issues a command whose response never arrives (dropped or invalid)
- **THEN** the helper returns a timeout/failure status after its bounded block budget instead of pumping forever
