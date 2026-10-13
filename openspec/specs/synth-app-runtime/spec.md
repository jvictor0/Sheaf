# synth-app-runtime Specification

Project: `projects/synth`. ID prefix: `sar`.

## Purpose

Define the synth application/runtime architecture: the JUCE-free application
contract (RuntimeConfig, AppContext, concepts), the shared Engine assembly and
pump, the JUCE runtime shell, runtime patch/MIDI/audio-device orchestration,
the headless SynthRig test harness, and the miniapp as the reference
runtime-hosted application.
## Requirements
### Requirement: sar-1 — Project: runtime and application layout
WHEN the synth application runtime capability is implemented, THE repository SHALL provide a JUCE-dependent runtime layer under `projects/synth/runtime` in namespace `synth_runtime`, an applications directory `projects/synth/apps` with one subdirectory per application, JUCE-free application contract headers (`RuntimeConfig`, `AppContext`, `AudioBlock`) under `projects/synth/include/synth` in namespace `synth`, and shared JUCE-module build rules in a makefile fragment included by each application's Makefile; the JUCE-free core library build and tests SHALL remain free of runtime and JUCE dependencies.

#### Scenario: Contract headers compile without JUCE
- **WHEN** a JUCE-free synth test includes the application contract headers
- **THEN** the test compiles without seeing `JUCE_MAJOR_VERSION`

#### Scenario: Application builds through shared scaffolding
- **WHEN** a developer runs the miniapp build target from `projects/synth`
- **THEN** the application under `projects/synth/apps/miniapp` builds using the shared JUCE build fragment and the runtime headers

#### Scenario: Core library unaffected
- **WHEN** a developer runs `make -C projects/synth build test`
- **THEN** the core library and its JUCE-free tests build and pass without compiling runtime or apps sources

### Requirement: sar-2 — Configuration: runtime config supplied by the application
WHEN a synth application is defined, THE application SHALL supply a JUCE-free `RuntimeConfig` value declaring at minimum the application name, audio input count, audio output count, preferred sample rate, preferred block size, and UI shell dimensions/frame rate; THE application SHALL NOT declare production patch roots, log roots, or persistent MIDI/audio device preferences through `RuntimeConfig`; THE runtime SHALL treat audio fields as a request, negotiate actual values with the audio device, and report the negotiated sample rate and block size to the application before audio processing starts.

#### Scenario: Config drives device request
- **WHEN** the runtime starts an application whose config requests 0 inputs, 2 outputs, and 48000 Hz
- **THEN** the runtime requests those settings when opening the audio device

#### Scenario: Negotiated values reported to the application
- **WHEN** the audio device opens with a sample rate or block size different from the preferred values
- **THEN** the application receives the actual negotiated sample rate and block size through its prepare hook before its first `ProcessBlock` call

#### Scenario: Application config does not own persistence paths
- **WHEN** an application returns its `RuntimeConfig`
- **THEN** the config contains no production patch-root or log-root fields
- **AND** the runtime resolves those paths from runtime-owned data paths instead

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
- **THEN** the runtime snapshots it as the default instrument restored by revert/new

#### Scenario: Clock is prepared from negotiated audio
- **WHEN** the host calls Engine prepare with its actual sample rate and block size
- **THEN** the retained master clock computes its increment and scheduling horizon from those negotiated values before the first application block

### Requirement: sar-4 — Application contract: compile-time interface
WHEN an application type is passed to the runtime template, THE runtime SHALL be a class template parameterized on the application type and SHALL verify at compile time (via C++20 concepts or equivalent static checks) a layered contract: a JUCE-free application-core concept requiring a runtime-config accessor, `Init(AppContext*)`, and a block-processing function, and a full application concept additionally requiring a UI-component hook; the JUCE runtime SHALL require the full concept while the engine and test rig SHALL require only the core concept; optional hooks (including the prepare hook and a control-rate frame hook) SHALL be detected at compile time and skipped when absent, and a type missing a required member SHALL fail compilation with a diagnostic naming the missing member.

#### Scenario: Conforming application instantiates
- **WHEN** the runtime template is instantiated with an application providing the required members
- **THEN** the program compiles and the runtime drives the application through its hooks

#### Scenario: Missing required member fails compilation
- **WHEN** the runtime template is instantiated with a type lacking `Init` or the block-processing function
- **THEN** compilation fails with a concept/static-assert diagnostic identifying the unmet requirement

#### Scenario: UI-less core is engine-hostable
- **WHEN** an application core type without a UI hook is passed to the engine or test rig
- **THEN** it compiles and runs headlessly
- **AND** passing the same type to the JUCE runtime fails compilation naming the missing UI hook

### Requirement: sar-5 — Lifecycle: construction, init ordering, and shutdown
WHEN the runtime starts, THE runtime SHALL construct the framework objects and application, then perform in order: application `Init(AppContext*)`; capture of default control state; creation and publication of the manager UI state; MIDI processor construction from the live instrument configuration; startup patch application, including the same per-controller MIDI-processor rebuild that runtime patch loads trigger when the loaded patch changes the instrument; startup connection of mapped controllers only after any such rebuild; start of the IO poll thread only after startup controller connection; audio device opening and invocation of the application prepare hook with the negotiated values; and only then registration of the audio callback and start of UI/message-thread timers. WHEN the runtime shuts down, THE runtime SHALL stop the audio callback before destroying the application, stop and join the IO poll thread before closing MIDI devices, stop and join the MIDI sender worker, and close open MIDI devices.

#### Scenario: Init precedes UI state and patch load
- **WHEN** the runtime starts an application
- **THEN** all parameter/module/page/bank registration performed by `Init` completes before the manager UI state is created
- **AND** the startup patch is applied after default control state capture

#### Scenario: Audio starts last
- **WHEN** the runtime finishes startup
- **THEN** the first audio callback runs only after init, UI-state creation, MIDI processor construction, and startup patch application have completed

#### Scenario: Patched instrument installed before controllers connect
- **WHEN** the startup patch carries a MIDI instrument configuration different from the application default
- **THEN** MIDI processors are rebuilt from the patched instrument before mapped controllers are connected

#### Scenario: Poll thread starts after initial connect
- **WHEN** the runtime finishes startup controller connection
- **THEN** the IO poll thread starts, and it did not run before that point

#### Scenario: Clean shutdown ordering
- **WHEN** the runtime shuts down
- **THEN** the audio callback is deregistered before the application and managers are destroyed
- **AND** the IO poll thread is stopped and joined before MIDI devices are closed
- **AND** the MIDI sender worker thread is stopped and joined
- **AND** open MIDI input and output devices are closed

### Requirement: sar-6 — Audio: device ownership and block delegation
WHEN audio is running, THE runtime SHALL own the JUCE audio device and its callback, and per device block SHALL apply pending patch messages (using an engine-owned preallocated patch serialization context whose arena growth and retry are owned by the message thread), process the UI and MIDI message buses into the parameter manager and runtime-owned master clock, commit one immutable affine `ClockBlockPlan` over the block's monotonic half-open output-sample range, analytically enqueue that plan's enabled MIDI clock crossings, and call the application's block-processing function exactly once with a JUCE-free audio block view exposing input pointers, output pointers, the actual output channel count, the host-observed active input count clamped to the application's requested count, frame count, monotonic starting output-sample index, and a non-owning pointer to the exact committed clock plan also returned by `MasterClock::CurrentPlan()`; zero-input blocks SHALL expose null input storage, and the plan pointer SHALL remain valid through the callback. THE runtime SHALL NOT use host audio block boundaries as the steady-state parameter target recomputation cadence, SHALL NOT call `Process` on any application DSP module, and SHALL NOT perform per-sample parameter processing — application per-sample work (parameter target refresh through the parameter system's configured sample interval, parameter `ProcessLite`, modulation-source updates, module processing, output writes) SHALL be owned by the application's block-processing function. Runtime clock work SHALL remain output-block-rate plus enumerated musical crossings, and applications SHALL obtain sample-accurate clock values by direct integer-or-fractional output-sample queries rather than advancing runtime time.

#### Scenario: Runtime commits clock then delegates
- **WHEN** an audio device block is processed
- **THEN** queued patch, UI, and MIDI messages are applied before the clock plan is committed and enabled crossings are enqueued
- **AND** the application's block-processing function is called exactly once with that immutable plan

#### Scenario: Control edits slew rather than snap
- **WHEN** an encoder message changes a parameter target while audio runs
- **THEN** the parameter's audible value approaches the new target through the parameter system's per-sample processing and `ProcessLite` slewing over subsequent samples rather than jumping in one block

#### Scenario: Application owns per-sample DSP processing
- **WHEN** the application's block-processing function runs
- **THEN** application module processing, per-sample parameter processing, and modulation-source updates are invoked by application code, not by runtime code
- **AND** the runtime-provided clock plan requires no application call to advance global time

#### Scenario: Clock plan is JUCE-free
- **WHEN** the application's block-processing code is compiled in a JUCE-free translation unit
- **THEN** the audio block and clock plan types compile without JUCE headers

#### Scenario: Block view exposes monotonic sample position
- **WHEN** the runtime calls the application's block-processing function for consecutive blocks
- **THEN** the second block's starting sample index equals the first block's starting sample index plus the first block's frame count

#### Scenario: App-owned oversampling maps into output time
- **WHEN** an application renders an internal sample at local oversampled index `i` and factor `F`
- **THEN** it may query the committed plan at `block.startSample + i / F` without informing the runtime of `F`
- **AND** the runtime does not call into the application once per internal sample

#### Scenario: Device channels are clamped to the app shape
- **WHEN** a host callback exposes more input channels than the application requested
- **THEN** the delegated block exposes only the requested prefix
- **AND** output delegation and monotonic sample-position behavior remain unchanged

### Requirement: sar-7 — Threading: ownership and queue handoff
WHILE audio is running, THE runtime SHALL treat the audio thread as the sole consumer of the UI, MIDI, and patch input buses and the sole thread that mutates or reads the parameter manager (including absolute-event processing acknowledgement and UI-state population at a throttled control cadence), SHALL keep the message thread as the sole producer of the UI and patch input buses and the sole thread performing parameter-storage-batch replies, patch manager responses and file IO, MIDI output processor polling, and MIDI device management, and SHALL keep MIDI input callbacks as the sole producer of the MIDI input bus; UI- and MIDI-originated messages SHALL be timestamped from one shared monotonic timestamp provider owned by the runtime, with timestamp-gated ordering guaranteed within each bus (cross-bus application order is by bus drain order within a block, not global timestamp order); the engine SHALL own one fixed-capacity thread-safe absolute-feedback coordinator whose runtime-local monotonic epoch allocator and per-controller-route expectation state survive controller processor rebuilds, whose input alert is linearized before the corresponding absolute message becomes visible to the audio consumer, and whose output decision is synchronized with that alert without requiring the audio thread to lock or allocate; patch command application MAY perform bounded non-real-time work (arena JSON serialization or parse, message payload destruction) at the block boundary as an accepted, user-initiated exception to the steady-state pump's allocation-free contract.

#### Scenario: Buses drained on the audio thread
- **WHEN** on-screen controls and MIDI hardware both enqueue messages while audio runs
- **THEN** both buses are processed into the parameter manager from the audio callback
- **AND** no message-thread code applies bus messages to the manager

#### Scenario: Storage growth handled off the audio thread
- **WHEN** the parameter message output bus reports a storage batch request
- **THEN** the message thread allocates and delivers the storage batch

#### Scenario: Shared timestamps order producers within a bus
- **WHEN** a UI control and a MIDI encoder produce messages in sequence
- **THEN** both messages carry timestamps from the runtime's shared monotonic provider
- **AND** each bus applies its own messages in timestamp order under the bus visibility rules

#### Scenario: Absolute feedback coordination crosses threads without entering DSP
- **WHEN** an absolute MIDI callback establishes an expected epoch while MIDI output polling and audio processing are active
- **THEN** the callback and message-thread output processor synchronize through the engine-owned coordinator
- **AND** the audio thread communicates completion only through its route epoch and coherent UI-state publication
- **AND** the audio thread neither locks the coordinator nor allocates memory

#### Scenario: Controller rebuild retains a pending epoch
- **WHEN** live configuration rebuilds input and output processors while an absolute epoch is awaiting DSP acknowledgement
- **THEN** the old processors may be replaced without destroying the engine-owned expectation
- **AND** the new output processor observes the pending epoch before sending position feedback

#### Scenario: Engine assembly identifies Generic profiles for derived output
- **WHEN** the engine builds processors for a Generic controller with encoder input and no explicit encoder output
- **THEN** it supplies the controller kind, stable controller-slot identity, and shared coordinator to profile construction
- **AND** the resulting Generic output uses each turn mapping's input channel and CC
- **AND** rebuilding processors preserves pending absolute expectations exactly as for explicit encoder outputs

### Requirement: sar-8 — Patches: runtime startup load and command orchestration
WHEN the runtime starts, THE runtime SHALL attempt to load the most recent patch under the runtime-owned patches root, selected deterministically by the library's sortable version-file naming — the patch directory containing the lexicographically greatest version filename, ties broken by directory name — and SHALL fall back silently to the application's initialized defaults when none exists; WHILE running, THE runtime SHALL expose new/save/save-as/load/revert patch commands through the patch manager, write and read version files only through library persistence helpers, and patch load/revert/new commands SHALL mutate synthesizer patch data only, not MIDI instrument configuration or audio device configuration.

#### Scenario: Startup loads the latest patch
- **WHEN** the runtime starts and the runtime-owned patches root contains saved patch directories
- **THEN** the parameter values from the version file with the lexicographically greatest name across patch directories are applied before audio starts
- **AND** MIDI instrument configuration and audio device configuration are not read from the patch

#### Scenario: Startup selection is deterministic under ties
- **WHEN** two patch directories contain version files with identical names
- **THEN** the directory with the lexicographically greater name is selected

#### Scenario: Missing startup patch keeps defaults
- **WHEN** the runtime starts with an empty patches root
- **THEN** the application's `Init` defaults remain in effect and no persistence failure is reported

#### Scenario: Load preserves runtime configuration
- **WHEN** a patch load message is consumed at runtime
- **THEN** the runtime applies saved parameter values from the patch
- **AND** keeps the current MIDI instrument configuration and audio device selection unchanged

### Requirement: sar-9 — MIDI: runtime device and instrument management
WHEN an application runs under the runtime, THE runtime SHALL own the MIDI sender lifecycle, per-controller MIDI device enumeration, open/close, and reconciliation handling (including the IO poll thread and message-thread reconciliation of smi-4), and construction of MIDI input/output processors for every controller in the live instrument configuration via the library profile factory, registering them against the MIDI message input bus and the manager UI state; applications SHALL NOT construct MIDI device handlers or processor chains directly.

#### Scenario: Profile factory wires processors per controller
- **WHEN** the runtime builds MIDI processors
- **THEN** each controller slot's processors are created through the library controller-profile factory from that slot's profile config
- **AND** registered against the MIDI input bus and the manager UI state

#### Scenario: Application stays free of device glue
- **WHEN** the miniapp application is inspected
- **THEN** it contains no MIDI device enumeration, open/close, reconciliation, or processor construction code

### Requirement: sar-10 — UI: runtime shell hosting an application component
WHEN a JUCE or browser runtime presents UI, THE runtime SHALL own its host-specific application/window or browser integration and SHALL render one shared JUCE-free runtime main component templated on the application and host services; that component SHALL host the library sidebar with Audio/Controllers/Sync/File pages and deadline readout plus the application's portable UI surface as default content, the runtime SHALL drive shared component refresh from manager and master-clock UI-state atomics at the configured frame rate, and each host entry point SHALL define its executable or static WASM application by naming only the application type.

#### Scenario: Shell hosts the complete application surface
- **WHEN** the JUCE window or Chrome static site opens
- **THEN** the application's complete portable UI surface is displayed as main-pane content beside the sidebar through the active backend
- **AND** no patch or MIDI chrome row sits above it
- **AND** the sidebar does not consume or clip the configured application content width

#### Scenario: Both hosts share top-level behavior
- **WHEN** sidebar navigation, runtime-page Back, or application actions are dispatched in JUCE and Chrome
- **THEN** both hosts route them through the same portable runtime main component
- **AND** host adapters contain no concrete-application UI or sync-policy behavior

#### Scenario: Repaint reads UI-state atomics
- **WHEN** the UI timer fires
- **THEN** sidebar, runtime Sync status, and application widgets render from published UI state without touching the parameter manager or mutable master-clock DSP state directly

#### Scenario: Typed application entry points
- **WHEN** a JUCE or browser application entry point names a conforming application type
- **THEN** the build produces the corresponding runtime-hosted application without application-specific host source

### Requirement: sar-11 — Miniapp: runtime-hosted reference application
WHEN the miniapp is ported to the runtime, THE miniapp application at `projects/synth/apps/miniapp` SHALL contain only application-specific content — runtime config, duophonic group and VCO/LFO/filter/ADSR module setup, page/bank/slot layout, scope wiring, per-sample block processing, clock-derived gate construction, and its bespoke widgets — SHALL preserve the existing specced miniapp behaviors (encoder grid, pages, scenes, gestures, MIDI controller configuration, patch commands, waveform pane), SHALL process its parameter group through the synth parameter system's group-level per-sample processing API using the audio block's monotonic sample index, SHALL expose the VCO page as module-backed VCO controls plus filter Cutoff, Resonance, and Blend controls, SHALL expose the LFO page and bank in order as five module-backed LFO parameters, ADSR Attack/Decay/Sustain/Release, and Tempo, SHALL copy `AdsrModule<2>::Outputs()` after each process call into an application-owned stable two-float mirror registered as one polyphonic modulation source at index 7 before the same-frame modulation-value update, and SHALL write its filtered VCO output to the negotiated audio device outputs using the device-provided sample rate.

#### Scenario: Miniapp init is application content only
- **WHEN** the miniapp sources are inspected
- **THEN** manager/bus/patch-manager/master-clock construction, message pumping, MIDI device glue, and patch orchestration are absent, provided instead by the runtime

#### Scenario: Miniapp produces audible filtered output
- **WHEN** the miniapp runs with an output-capable audio device
- **THEN** the filtered VCO voices are written to the device output channels
- **AND** the VCO, filter, LFO, and ADSR modules use the negotiated device sample rate

#### Scenario: Miniapp VCO page exposes filter controls
- **WHEN** the miniapp VCO page is active
- **THEN** the selected slot exposes Tune, Phase, Shape, Volume, Cutoff, Resonance, and Blend in that visible order
- **AND** Cutoff, Resonance, and Blend come from `ClassicSvfModule<2>`

#### Scenario: Miniapp LFO page is module-backed
- **WHEN** the miniapp LFO page is active
- **THEN** the selected slot exposes Frequency, Shape, Phase Offset, Skew, Exponent, Attack, Decay, Sustain, Release, and Tempo in that visible order
- **AND** the first five controls come from `BasicLfoModule<2>`, the next four come from `AdsrModule<2>`, and Tempo maps 30–300 BPM with a 120 BPM default

#### Scenario: Miniapp ADSR gate follows transport quarters
- **WHEN** transport is Running
- **THEN** MiniApp queries the committed plan at each frame's absolute output-sample position and both ADSR voices receive a high gate for transport quarter-note phase `[0, 0.5)` and a low gate for `[0.5, 1.0)` modulo one
- **AND** every integer quarter-note boundary retriggers an eighth-note-long gate
- **WHEN** transport Stops
- **THEN** the next processed ADSR sample receives gate low

#### Scenario: Miniapp ADSR is a modulation source
- **WHEN** MiniApp processes an audio frame
- **THEN** it processes `AdsrModule<2>` before the group modulation-value update
- **AND** the current per-voice envelope outputs are available from one connected application modulator index in that same frame

#### Scenario: Miniapp tempo control uses clock authority
- **WHEN** MiniApp's effective Tempo parameter changes with receive-clock disabled
- **THEN** it calls the master-clock tempo API and the new manual tempo applies no later than the next audio block
- **WHEN** receive-clock is enabled
- **THEN** the same control call is ignored by MasterClock and external tempo remains authoritative

#### Scenario: Miniapp uses parameter-owned compute cadence
- **WHEN** the miniapp's per-sample block processing is inspected
- **THEN** it calls the synth parameter system's group-level per-sample processing API with `block.startSample + frame`
- **AND** it does not separately schedule steady-state `Compute()` or `ComputeAllTargets()` from host block size

#### Scenario: Behavior parity with the probe app
- **WHEN** the ported miniapp runs
- **THEN** the existing miniapp scenarios for encoders, modulation views, scenes, gestures, MIDI configuration, and patch save/load continue to hold

### Requirement: sar-12 — Engine: shared JUCE-free assembly and pump
WHEN the runtime layer is implemented, THE system SHALL factor all non-JUCE-bound runtime behavior into a JUCE-free engine class template under `projects/synth/include/synth` that owns the framework objects, application instance, context, and sample counter, and exposes the initialization lifecycle (excluding device, window, and MIDI-device steps), a separate prepare entry point that the host invokes with negotiated (or, for headless hosts, configured) sample rate and block size after device negotiation, the per-block audio pump, and the message-side tick (storage-batch replies, patch responses, MIDI output polling) with an injectable timestamp provider; THE JUCE runtime SHALL delegate its audio callback and message-thread duties to this engine so runtime and headless hosts execute identical production code.

#### Scenario: Engine compiles without JUCE
- **WHEN** a JUCE-free test includes the engine header and instantiates it with an application core
- **THEN** it compiles and initializes without seeing `JUCE_MAJOR_VERSION`

#### Scenario: Runtime delegates to the engine
- **WHEN** the JUCE runtime processes an audio device block or a message-thread timer tick
- **THEN** the block pump and message-side duties execute through the engine's entry points rather than duplicated runtime code

### Requirement: sar-13 — Test rig: headless SynthRig harness
WHEN system-level tests need to drive an assembled application, THE repository SHALL provide a JUCE-free `SynthRig` test harness under `projects/synth/tests/support` that wraps the engine with an application core and provides: deterministic time driving (`RunBlocks`, `RunSamples`, `RunSeconds`) that pumps audio blocks with block-derived timestamps and runs the message-side tick each block on the single test thread; message injection through production paths (encoder turn/press/shift, gesture, scene, and blend messages onto the UI bus, and raw MIDI messages through the engine's MIDI input processor chain into the MIDI bus); observation of manager UI state, parameter values, and captured output with sticky NaN/Inf and peak invariants over every output sample; and patch save/load/revert helpers that issue patch manager commands and pump for a bounded number of blocks, returning a status that distinguishes success, command failure, and timeout rather than hanging.

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

### Requirement: sar-14 — Miniapp: headless system-test coverage
WHEN the miniapp is ported, THE miniapp SHALL be structured as a JUCE-free application core plus a thin UI wrapper, and THE synth test suite SHALL include a rig-hosted miniapp system test that initializes the core through the engine, runs blocks, drives encoder and scene/gesture messages, verifies audio output renders without NaN with nonzero peak when the VCO volume is raised, verifies the module-backed LFO page exposes and routes five parameters, and round-trips a patch save/load through the production message flow.

#### Scenario: Miniapp core tests run JUCE-free
- **WHEN** a developer runs `make -C projects/synth test`
- **THEN** the rig-hosted miniapp system test builds without JUCE and passes as part of the suite

#### Scenario: Headless LFO page routes five parameters
- **WHEN** the rig selects the LFO page
- **THEN** the selected slot contains Frequency, Shape, Phase Offset, Skew, and Exponent controls
- **AND** turning each visible LFO control changes the corresponding parameter value through the production message path

#### Scenario: Headless patch round-trip
- **WHEN** the rig test edits parameters, saves a patch, perturbs state, and loads the saved patch
- **THEN** the loaded parameter values match the saved values through the production patch message flow

### Requirement: sar-15 — Audio: device selection and patch persistence
WHEN the runtime presents configuration UI, THE runtime SHALL provide audio device selection through the library Audio page (sru-3) (output device, and input device when the application requests inputs) instead of always using the system default; the selection SHALL be held as a JUCE-free audio device state (empty meaning system default) that persists in the runtime configuration document and round-trips through configuration save and load like the MIDI instrument state; WHEN a loaded startup configuration names an audio device, THE runtime SHALL switch to that device when it is present (preparing the engine with the negotiated values) and SHALL keep the current/default device with a visible status, not a startup failure, when it is absent.

#### Scenario: User selects a non-default interface
- **WHEN** the user picks an audio output device from the Audio page
- **THEN** the audio device manager switches to that device on the message thread
- **AND** the engine is re-prepared with the device's actual sample rate and block size

#### Scenario: Device selection round-trips through configuration
- **WHEN** runtime configuration saved with a named output device is loaded while that device is present
- **THEN** the runtime switches to the named device during startup

#### Scenario: Patch load does not switch audio device
- **WHEN** a patch is loaded after the user has selected an audio device
- **THEN** the current audio device selection remains unchanged by the patch load

#### Scenario: Absent device degrades gracefully
- **WHEN** loaded runtime configuration names an audio device that is not currently present
- **THEN** the current/default device keeps running and the status reports the missing device
- **AND** startup itself succeeds

### Requirement: sar-16 — Patches: message-side identity and save fallback
WHEN the runtime tracks which patch is current, THE current patch identity (directory, name, and pending-save state) SHALL be owned by message-thread components (the patch manager and runtime shell) and SHALL NOT be cached on the audio side; THE library File page (sru-6) SHALL display the current patch name and command status, updating as commands complete; WHEN the user invokes Save while no current patch directory exists, THE File page SHALL fall through to the Save As flow rather than surfacing a needs-path failure.

#### Scenario: File page shows the current patch
- **WHEN** a patch is saved as or loaded from a directory
- **THEN** the File page displays that patch's name

#### Scenario: First save falls through to Save As
- **WHEN** the user presses Save before any patch directory exists
- **THEN** the Save As chooser opens instead of a needs-path error
- **AND** completing it writes the first version file and sets the current patch

### Requirement: sar-17 — Data paths: runtime-owned persistent app data
WHEN a runtime-hosted synth application starts, THE runtime SHALL resolve a persistent app data root owned by the runtime host, create or use `patches/` and `logs/` subdirectories under that root, expose the resolved paths to the engine and runtime pages as immutable runtime data paths, and use an OS-appropriate long-lived user application data location for production runs rather than a temporary directory.

#### Scenario: Production data root is long-lived
- **WHEN** the JUCE runtime starts the miniapp in production mode
- **THEN** its data root is under the user's application data area for Sheaf and the app name
- **AND** the root is not under the system temporary directory or the build tree

#### Scenario: Runtime creates standard subdirectories
- **WHEN** the runtime resolves data paths for an app
- **THEN** `patches/` and `logs/` exist or are created under the data root
- **AND** the File page and patch manager use `patches/` as the only production patch root
- **AND** the async logger uses `logs/` as the production log root

#### Scenario: Tests can inject scratch paths
- **WHEN** a headless rig or unit test constructs a runtime/engine host with explicit data paths
- **THEN** the host uses those scratch paths instead of the production OS data directory

### Requirement: sar-24 — UI: portable application surface
WHEN a synth application exposes user interface content to a host runtime, THE application SHALL expose that content through a JUCE-free synth-owned portable UI contract rather than returning or naming `juce::` component, graphics, event, string, colour, or geometry types from application-facing headers; the JUCE desktop runtime SHALL adapt that portable UI surface to JUCE components behind a JUCE-owned backend, and the existing engine, audio, MIDI, patch, and message-bus semantics SHALL remain unchanged.

#### Scenario: Application UI headers compile without JUCE
- **WHEN** a JUCE-free synth test includes the application UI contract headers and the miniapp's application-facing UI headers without JUCE include paths
- **THEN** the test compiles without seeing `JUCE_MAJOR_VERSION`
- **AND** those headers expose no `juce::` types in their public signatures

#### Scenario: Desktop runtime adapts portable UI
- **WHEN** the JUCE desktop runtime starts the miniapp
- **THEN** it constructs the app's portable UI surface and hosts it through the JUCE backend adapter
- **AND** audio startup, MIDI startup, patch startup load, message-thread ticking, and shutdown ordering continue to follow the existing runtime lifecycle

#### Scenario: Miniapp visual behavior is preserved
- **WHEN** the miniapp is run through the JUCE desktop backend after the refactor
- **THEN** the encoder grid, waveform panels, bank buttons, gesture controls, scene controls, modifier buttons, start/stop controls, and runtime sidebar/pages remain visibly and behaviorally equivalent to the pre-refactor miniapp

#### Scenario: JUCE remains isolated to backend code
- **WHEN** source files outside explicitly JUCE-owned backend/runtime host paths are inspected
- **THEN** application widgets, portable UI interfaces, miniapp UI layout/drawing logic, and synth core tests do not include JUCE headers or refer to `juce::` symbols

### Requirement: sar-18 — Configuration: runtime startup load and save ownership
WHEN the runtime starts, THE runtime SHALL initialize the application-defined synth topology first, then load the runtime configuration document from the runtime-owned data root if it exists, applying MIDI instrument/controller configuration, audio device selection, and sync configuration before MIDI processors are built, controller reconciliation starts, the master clock is prepared for output, or the audio device is opened; WHEN the runtime is asked to save configuration, THE runtime SHALL write the current MIDI instrument/controller configuration, audio device selection, and sync configuration to that document without writing synthesizer patch data.

#### Scenario: Missing configuration keeps app defaults
- **WHEN** the runtime starts and no runtime configuration document exists
- **THEN** the MIDI instrument and audio device state established by application initialization and system defaults remain active
- **AND** sync uses all-disabled flags with PPQN 24
- **AND** startup continues without reporting a persistence failure

#### Scenario: Configuration loads before controller reconciliation
- **WHEN** the runtime configuration document contains a MIDI instrument with controller endpoint references and non-default sync settings
- **THEN** MIDI processors are built from that loaded instrument
- **AND** startup controller reconciliation uses the loaded endpoint references
- **AND** the master clock uses the loaded sync settings before its first processed audio block

#### Scenario: Configuration save excludes patch state
- **WHEN** runtime configuration is saved after Audio, Controllers, or Sync page edits
- **THEN** the saved document contains MIDI instrument/controller configuration, audio device state, and sync configuration
- **AND** it does not contain patch parameter values or patch identity

### Requirement: sar-19 — Apps: manifest metadata and registry
WHEN synth applications are made available to the Sheaf Patch launcher, THE synth application runtime SHALL provide a JUCE-free app registration contract that binds a concrete app type to manifest metadata containing a stable app id, display name, author, category, and hardware requirements including minimum encoder count.

#### Scenario: Miniapp has launcher metadata
- **WHEN** the app registry is built for the Sheaf Patch launcher
- **THEN** it contains the miniapp registration
- **AND** that registration declares category `test`
- **AND** it declares a minimum encoder count in its hardware requirements

#### Scenario: Metadata is separate from runtime config
- **WHEN** a registered app's runtime configuration is requested
- **THEN** device configuration fields still come from `RuntimeConfig`
- **AND** launcher display fields and hardware requirements come from the app manifest metadata

#### Scenario: App id is path-safe
- **WHEN** an app is registered
- **THEN** its stable app id is valid for use as one path segment below the Sheaf Patch patches directory
- **AND** the runtime rejects or fails to compile registrations whose app id is empty

### Requirement: sar-20 — Apps: typed launch binding
WHEN a launcher-visible app is registered, THE synth application runtime SHALL provide a typed launch binding that constructs the selected app's runtime, applies caller-supplied runtime data paths, and starts the runtime without requiring the launcher to know app-specific initialization, audio, MIDI, patch, or UI internals.

#### Scenario: Launcher launches through registration
- **WHEN** the user selects a registered app from the Sheaf Patch launcher
- **THEN** the launcher invokes that registration's launch binding
- **AND** the binding constructs and starts the runtime for the registered app type

#### Scenario: Launcher stays thin
- **WHEN** the Sheaf Patch launcher source is inspected
- **THEN** it contains app list and selection code
- **AND** it does not construct parameter managers, patch managers, MIDI processors, audio devices, app module graphs, or app-specific UI widgets directly

#### Scenario: Missing app contract is caught at build time
- **WHEN** a registration is attempted for a type that does not satisfy the synth application concept
- **THEN** compilation fails with a diagnostic naming the unmet app contract

### Requirement: sar-21 — Product: Sheaf Patch superapp executable
WHEN the Sheaf Patch product is built, THE repository SHALL provide a top-level `sheaf-patch` synth executable that opens to the launcher instead of immediately starting a specific synth app, and that can launch the registered miniapp as its first contained app.

#### Scenario: Superapp starts at launcher
- **WHEN** the `sheaf-patch` executable starts
- **THEN** the first visible screen is the launcher app list
- **AND** no synth app runtime has started until the user selects an app

#### Scenario: Miniapp remains launchable
- **WHEN** the user selects the miniapp in the `sheaf-patch` launcher
- **THEN** the miniapp runtime starts through the same runtime lifecycle used by standalone runtime-hosted apps

#### Scenario: Standalone miniapp target remains available
- **WHEN** the existing miniapp build target is built
- **THEN** it continues to produce a standalone runtime-hosted miniapp unless a later change explicitly removes that target

### Requirement: sar-22 — Apps: type-erased ownership of typed runtime sessions
WHEN Sheaf Patch launches any registered synth application, THE synth runtime SHALL construct the application's typed `RuntimeShellSession<App>` behind a minimal type-erased session-owner interface that preserves component access, deterministic destruction, and the existing runtime shutdown order, so the Sheaf Patch application object does not need an app-specific runtime-session field or launch method for each registered app type.

#### Scenario: Existing MiniApp uses generic session ownership
- **WHEN** the user launches MiniApp from Sheaf Patch
- **THEN** the generic session owner contains a `RuntimeShellSession<MiniApp>`
- **AND** Sheaf Patch displays its component through the type-erased interface

#### Scenario: A second app needs no launcher-specific runtime member
- **WHEN** Braid 4 is registered
- **THEN** the Sheaf Patch application object still owns exactly one generic active-session pointer
- **AND** it declares no `RuntimeShellSession<Braid4>` member or Braid-specific launch method

#### Scenario: Replacing or destroying a session preserves lifecycle
- **WHEN** the active session owner is destroyed during application shutdown
- **THEN** the contained typed runtime session performs the same audio, MIDI, timer, and application shutdown ordering as a directly owned session

### Requirement: sar-23 — Apps: Braid 4 Sheaf Patch registration
WHEN the Sheaf Patch registry is built, THE synth application runtime SHALL include a typed Braid 4 registration with stable app id `braid-4`, display name `Braid 4`, author `Sheaf`, category `synth`, and advisory minimum encoder count `16`, and SHALL launch it with the shared Sheaf Patch configuration path plus app-specific `patches/braid-4` path without requiring a standalone Braid executable.

#### Scenario: Launcher metadata identifies Braid
- **WHEN** the Sheaf Patch launcher lists registered applications
- **THEN** one row contains id `braid-4`, display name `Braid 4`, author `Sheaf`, category `synth`, and minimum encoders `16`

#### Scenario: Braid launch uses typed binding
- **WHEN** the user activates the Braid 4 row
- **THEN** the registration constructs and starts the Braid 4 runtime through the generic session owner
- **AND** the launcher does not construct Braid parameters, modules, UI, audio, or MIDI internals

#### Scenario: Braid patches are isolated
- **WHEN** Braid 4 is launched from Sheaf Patch
- **THEN** runtime configuration uses the shared Sheaf Patch config file
- **AND** patch discovery and save/load use the `patches/braid-4` subtree

#### Scenario: Standalone target is not required
- **WHEN** the synth application targets are inspected or built
- **THEN** Sheaf Patch includes Braid 4
- **AND** no standalone Braid 4 `Main.cpp`, executable, or app-bundle target is required

### Requirement: sar-26 — Apps: build-time external application injection
WHEN the Sheaf Patch executable is built, THE synth application runtime SHALL support adding one out-of-tree synth application to the launcher registry through build configuration alone, SHALL require the injecting build to supply that application's include directory, header, application type, registration factory, and header dependency list together, and SHALL produce an unchanged Sheaf Patch build when no external application is configured. The external application SHALL reach the registry through the same manifest contract, typed launch binding, and generic session ownership as in-tree registrations, and the launcher SHALL NOT gain app-specific knowledge of it.

#### Scenario: Unconfigured build is unchanged
- **WHEN** Sheaf Patch is built with no external application configured
- **THEN** the launcher registry contains only the in-tree registrations
- **AND** no external-application preprocessor definition reaches the compiler

#### Scenario: Configured external application joins the registry
- **WHEN** Sheaf Patch is built with a complete external application configuration
- **THEN** the launcher lists the external application alongside the in-tree registrations
- **AND** selecting it constructs and starts its runtime through the typed launch binding and the generic session owner
- **AND** the launcher declares no member, launch method, or UI specific to that application

#### Scenario: Partial configuration fails the build
- **WHEN** an external application is configured with any required build variable missing
- **THEN** the build fails with a diagnostic naming the missing variable
- **AND** no partially configured executable is produced

#### Scenario: External headers participate in rebuild detection
- **WHEN** a declared external application header changes
- **THEN** the next Sheaf Patch build recompiles rather than reporting the executable up to date

### Requirement: sar-25 — Control topology: runtime-owned button grids
WHEN a synth application runs under the engine runtime, THE runtime SHALL own one `GridManager` as a sibling of its `ParameterManager`, SHALL attach both the UI and MIDI `MessageInBus` instances to both managers, SHALL finalize and allocate grid UI topology during initialization, SHALL drain grid and parameter messages through the existing ordered bus pumps before application block processing, SHALL publish parameter and selected-grid state into one stable internal runtime UI snapshot used by MIDI processors, and SHALL destroy the grid manager only after audio and MIDI processing have stopped; this change SHALL NOT require an existing application to create, expose, or render a grid.

#### Scenario: Grid manager follows runtime lifetime
- **WHEN** an engine is constructed, initialized, run, and destroyed
- **THEN** exactly one grid manager exists beside the parameter manager for the engine lifetime
- **AND** its cells and UI state are destroyed only after audio and MIDI callbacks can no longer access them

#### Scenario: Initialization allocates before processors start
- **WHEN** runtime initialization completes
- **THEN** grid topology is finalized and its UI state is allocated before the first MIDI processor rebuild, audio callback, or UI publication

#### Scenario: Both buses can route both control families
- **WHEN** UI and MIDI producers enqueue interleaved parameter and grid messages
- **THEN** each bus applies both families before the application processes that audio block
- **AND** each family reaches only its corresponding manager

#### Scenario: MIDI processors receive global state
- **WHEN** the runtime builds a controller profile containing encoder and grid feedback mappings
- **THEN** encoder output reads the parameter portion of the runtime snapshot
- **AND** grid system output reads the grid portion of the same stable snapshot facade

#### Scenario: Existing apps require no grid integration
- **WHEN** an application defines only parameter groups, banks, and existing MIDI configuration
- **THEN** it initializes and runs with the same application contract and parameter UI-state access as before
- **AND** no application grid component or main-surface control appears

### Requirement: sar-27 — Hosts: compile-time swappable runtime shells
WHEN a synth application is hosted by a platform runtime, THE synth runtime architecture SHALL allow multiple compile-time runtime shells to instantiate the same `synth::Engine<App>` and `synth::SynthApplication` type without changing application source; each host shell SHALL own only platform integration for audio devices, MIDI devices, UI presentation, persistence mounting, timers, and process/window lifecycle, while engine, application, patch, parameter, MIDI-processor, and portable UI semantics remain shared.

#### Scenario: Same app type binds to multiple hosts
- **WHEN** a conforming application type is named by the JUCE runtime entry point and by the browser runtime entry point
- **THEN** both targets compile against the same application headers
- **AND** neither target requires app-specific runtime glue outside the generic host binding

#### Scenario: Host-specific APIs stay in host paths
- **WHEN** runtime host sources are inspected
- **THEN** JUCE APIs appear only in JUCE-owned host/backend paths
- **AND** browser APIs or JavaScript bindings appear only in browser-owned host/backend paths
- **AND** synth application sources include neither host API family

### Requirement: sar-28 — Hosts: application portability gate
WHEN a new runtime host is implemented for synth applications, THE implementation SHALL prove that at least one fake application and each targeted concrete application satisfy the generic `SynthApplication` contract without host-specific code; IF a targeted concrete application cannot be hosted without app-specific runtime/backend logic, THEN implementation SHALL stop and document the missing generic contract rather than adding the special case.

#### Scenario: Fake app proves generic host path
- **WHEN** host conformance tests instantiate a fake application with config, init, process-block, and portable-surface hooks
- **THEN** the host compiles and drives the fake application through the same generic runtime path used for concrete apps

#### Scenario: Concrete app portability failure blocks implementation
- **WHEN** the browser host attempts to run the miniapp or any other concrete app and discovers a required operation that is not expressible through `AppContext`, `Engine`, MIDI profile factories, patch/config APIs, or `synth::ui::Surface`
- **THEN** the browser change is considered blocked
- **AND** no app-specific browser branch is added

### Requirement: sar-29 — Audio: host-owned device callback abstraction
WHEN a non-JUCE runtime host provides audio, THE host SHALL adapt its platform audio callback into the JUCE-free `AudioBlock` contract, call `Engine<App>::Prepare` with the platform-negotiated sample rate and actual callback/render quantum before the first processed block, call `Engine<App>::ProcessBlock` exactly once for each callback/render quantum, and preserve monotonic sample-position semantics independent of the platform's native callback representation.

#### Scenario: Browser host uses native Web Audio callback values
- **WHEN** the browser runtime starts its native Wasm AudioWorklet callback
- **THEN** the engine has been prepared with the `AudioContext` sample rate and the actual Web Audio render quantum
- **AND** `ProcessAudioWorklet` delegates through `Runtime<App>::Process` to give the app a JUCE-free output `AudioBlock`

#### Scenario: Host callback does not own DSP
- **WHEN** any platform host processes an audio block
- **THEN** the host performs platform buffer adaptation and delegates to the engine/app block path
- **AND** it does not call app DSP modules or parameter processing directly

### Requirement: sar-30 — Audio: host-provided device catalog
WHEN the runtime Audio page needs audio device choices, THE synth runtime architecture SHALL obtain those choices from the active host shell through a generic audio-device catalog interface, so JUCE hosts may expose enumerated native devices while browser hosts expose System Default choices without changing application logic, runtime page logic, or patch/runtime configuration semantics.

#### Scenario: Browser catalog follows the input request
- **WHEN** a browser-hosted application requests zero inputs
- **THEN** the page receives `showInputCombo == false` and an empty input option list
- **AND** selecting the System Default output commits the existing empty persisted output-device name
- **WHEN** a browser-hosted application requests one or more inputs
- **THEN** the page receives `showInputCombo == true` and exactly one input option labeled `System Default` with option id `system_default`
- **AND** selecting that option commits the existing empty persisted input-device name
- **AND** any other browser input option id is rejected

#### Scenario: JUCE host keeps native device enumeration
- **WHEN** the Audio page is hosted by the JUCE runtime
- **THEN** the page can continue to list host-enumerated input and output devices according to the application request
- **AND** the application source does not branch on the active host

### Requirement: sar-31 — Audio: requested inputs and portable views
WHEN an application declares a `RuntimeConfig::numAudioInputs` value `N`, THE synth runtime SHALL reject negative values before engine or device startup, accept any representable nonnegative value on JUCE and any value up to the browser host's explicit 32-channel limit, request `N` logical input channels from the active host, expose no more than `N` active channels in each `AudioBlock`, and provide JUCE-free trivially-copyable non-owning input views for contiguous channel blocks and cross-channel sample frames without allocation, ownership, or sample copying; the block SHALL expose the actual active channel count, strict access SHALL require an active non-null channel, explicitly safe access SHALL return silence for a missing channel, null channel pointer, or invalid frame, and callers SHALL NOT retain any view beyond its callback.

#### Scenario: Negative input request is rejected uniformly
- **WHEN** any host or `SynthRig` encounters an application config with a negative input count
- **THEN** the shared JUCE-free validation raises `std::invalid_argument` before engine preparation, device opening, capture, or AudioWorklet startup
- **AND** a C or JavaScript ABI boundary translates that failure into an explicit startup diagnostic

#### Scenario: Multichannel request has no framework ceiling
- **WHEN** a conforming application requests 17 input channels
- **THEN** the JUCE-free application/runtime contract accepts the count without imposing a synth-framework fixed-array maximum
- **AND** JUCE accepts the representable request while the browser accepts requests through 32 and rejects 33 or more with an explicit platform-limit diagnostic before capture or audio starts

#### Scenario: Channel and frame views address the same samples
- **WHEN** a block contains distinct finite samples across multiple input channels and frames
- **THEN** reading sample `(channel, frame)` through the channel-block view and through the cross-channel frame view returns the same underlying value
- **AND** constructing or reading either view performs no allocation or input-buffer copy

#### Scenario: Input views are structurally non-owning
- **WHEN** contract checks inspect the input block and frame view types
- **THEN** each type is trivially copyable, contains no owning storage, and has a bounded size independent of channel or frame count
- **AND** its documentation marks the view invalid after the callback returns

#### Scenario: Device exposes fewer channels than requested
- **WHEN** an application requests `N` inputs and the active host provides `M < N` channels
- **THEN** `AudioBlock::numInputChannels` reports `M`
- **AND** strict access is available for non-null channels `0..M-1`
- **AND** strict access to a counted null channel is a documented precondition violation rather than a defined sample return
- **AND** explicitly safe access for channels `M..N-1` returns zero
- **AND** the host reports a requested-versus-active channel diagnostic while output processing continues

#### Scenario: Counted null channel is safe silence
- **WHEN** a host reports a channel inside the counted input range with a null sample pointer
- **THEN** its logical channel position is preserved
- **AND** strict access documents that the channel is unavailable
- **AND** explicitly safe access returns zero without dereferencing the pointer

#### Scenario: Extra device channels are not injected
- **WHEN** the selected device exposes more active channels than the application-requested count
- **THEN** the application block exposes only the first requested number of logical input channels
- **AND** changing hardware does not silently expand the application's input shape

#### Scenario: Zero-input application does not capture
- **WHEN** an application declares zero audio inputs
- **THEN** the active host opens or constructs no input path for that application
- **AND** the application receives a block with `inputs == nullptr`, zero active input channels, and an empty input view

#### Scenario: Input is not implicitly monitored
- **WHEN** nonzero samples arrive on an active input and application DSP does not copy or otherwise incorporate them into output
- **THEN** the runtime leaves output determined solely by application output writes
- **AND** the host creates no direct input-to-output monitoring path

### Requirement: sar-32 — Test rig: deterministic multichannel audio input
WHEN JUCE-free system tests exercise an input-capable synth application, THE `SynthRig` SHALL allocate block-sized planar storage for the application's declared input count before processing begins, initialize it to silence, provide validated helpers for deterministic channel, frame, sample, and complete-block injection without adding allocations beyond the rig's existing capture-vector path, and pass the configured input through the production `Engine<App>::ProcessBlock` path without allocating in the block pump.

#### Scenario: Rig is silent until input is injected
- **WHEN** an input-capable rig runs before a test supplies samples
- **THEN** every declared input channel reads as zero
- **AND** the app still observes the declared rig input storage through the production audio-block contract

#### Scenario: Injected channels reach application DSP
- **WHEN** a test injects distinct sample sequences into multiple rig input channels and runs one block
- **THEN** the application receives those sequences with the same channel and frame ordering
- **AND** an application that explicitly transforms them into output produces the expected transformed output

#### Scenario: Invalid injection is rejected before processing
- **WHEN** a test attempts to inject a channel, frame, or block shape outside the rig's configured input dimensions
- **THEN** the helper rejects the operation deterministically
- **AND** no partial input mutation reaches the next processed block

### Requirement: sar-42 — Data paths: a host that keeps no runtime configuration
WHERE a host's runtime data paths name a patches root and no configuration file, THE engine SHALL load no runtime configuration, SHALL open no startup patch, and SHALL write no configuration file, at startup or after a patch command; a host whose configuration file is named but does not exist SHALL keep the startup behaviour of `sar-8` and `sar-18`.

#### Scenario: No startup patch without a configuration file
- **WHEN** the engine initializes, under a catalog that sets `patchCarriesMappings`, with a patches root holding a saved patch whose parameter values differ from the defaults and whose instrument section holds a controller, and with no configuration file named
- **THEN** after initialization and one message-thread tick, every parameter holds its default value and the live instrument equals the application's initial instrument
- Check: not yet delivered: NEW engine_initialize_without_a_configuration_file_opens_no_startup_patch in tests/engine_tests.cpp (frogg3rs task S1)

#### Scenario: A named but missing configuration keeps startup loading
- **WHEN** the engine initializes with a configuration file named but absent and a saved patch present
- **THEN** the newest patch opens
- Check: `tests/engine_tests.cpp`, `engine_initialize_treats_missing_runtime_config_as_defaults_and_still_loads_startup_patch`

