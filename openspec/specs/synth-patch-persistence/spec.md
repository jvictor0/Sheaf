# synth-patch-persistence Specification

## Purpose
TBD - created by archiving change add-midi-profile-json-persistence. Update Purpose after archive.
## Requirements
### Requirement: spp-1 — JSON: arena-backed tree
WHEN synth patch persistence needs JSON parsing or serialization, THE synth patch persistence system SHALL provide a JUCE-free arena-backed JSON library whose nodes, object members, array entries, object keys, and string values allocate only from a caller-owned `JsonArena`, whose build and read operations are null-tolerant after arena exhaustion, and whose parser and serializer round-trip standard patch JSON text.

#### Scenario: Arena builds object tree
- **WHEN** code builds an object with strings, integers, reals, booleans, arrays, and nested objects through `JsonArena`
- **THEN** all JSON nodes and stored strings are allocated from that arena
- **AND** the resulting tree can be serialized to valid JSON text

#### Scenario: Exhaustion is recoverable
- **WHEN** the arena is too small for a JSON build or parse
- **THEN** the arena records failure without crashing
- **AND** callers can grow/reset the arena and retry the same operation

### Requirement: spp-2 — Patch document format
WHEN a synth patch is saved, THE synth patch persistence system SHALL write a JSON object containing a synth patch schema identifier, schema version, patch name, and recursive parameter values keyed by initialized parameter name, while excluding MIDI instrument/controller configuration, audio device selection state, parameter definitions, and synth topology from persisted patch JSON; if legacy patch JSON contains MIDI or audio configuration sections, patch validation SHALL tolerate those extra sections without applying them as patch state.

#### Scenario: Patch root has required sections
- **WHEN** a patch is serialized
- **THEN** the JSON root contains `schema`, `schemaVersion`, `patchName`, and `parameterValues`
- **AND** does not contain `midiInstrument` or `audioDevice`

#### Scenario: Patch root scopes to one initialized manager
- **WHEN** a patch document stores `parameterValues`
- **THEN** those values apply to exactly one initialized `ParameterManager`
- **AND** top-level parameter names are interpreted only within that manager's existing parameter-name uniqueness contract

#### Scenario: Definitions are not persisted
- **WHEN** the patch stores `parameterValues`
- **THEN** it stores value state for initialized parameters by name
- **AND** it does not store groups, pages, banks, slots, modules, parameter names, colors, ranges, polarity, modulation-source metadata, or modulation assignments

#### Scenario: Legacy configuration sections are ignored
- **WHEN** a patch document contains legacy `midiInstrument` or `audioDevice` sections
- **THEN** patch validation succeeds if the patch schema and parameter values are otherwise valid
- **AND** loading the patch leaves current MIDI instrument configuration and audio device state unchanged

### Requirement: spp-3 — Patch file version history
WHEN synth patch persistence saves patch JSON to disk, THE system SHALL store each patch in its own directory under a configurable patches root and SHALL create one new sortable JSON version file per save without overwriting older versions.

#### Scenario: Save creates version file
- **WHEN** patch `BrightLead` is saved twice
- **THEN** both saves are written under `patches/BrightLead/`
- **AND** the second save does not overwrite the first save

#### Scenario: Save explicit patch directory creates version file
- **WHEN** a caller saves JSON text to an explicit patch directory
- **THEN** the persistence system creates one version file directly in that directory
- **AND** does not derive a second directory name from `patchName`

#### Scenario: Latest load uses sortable filename
- **WHEN** a patch directory contains multiple JSON version files
- **THEN** loading the latest patch version selects the alphanumerically greatest version filename

#### Scenario: Explicit version load
- **WHEN** a caller provides an explicit patch version file path under a patch directory
- **THEN** the persistence system loads that file rather than the latest version

### Requirement: spp-4 — Patch save and load APIs
WHEN application code requests synth patch save or load without a UI, THE synth patch persistence system SHALL expose JUCE-free library APIs to serialize initialized parameter values to JSON, parse patch JSON with a caller-owned arena, and apply only matching named parameter values to an already initialized parameter manager.

#### Scenario: Programmatic save returns JSON
- **WHEN** tests, reusable synth callers, or miniapp code request a patch save for an initialized synth instance
- **THEN** the persistence API returns or writes a JSON patch document without requiring a visible UI
- **AND** the patch document excludes MIDI instrument/controller configuration and audio device selection

#### Scenario: Programmatic load tolerates app changes
- **WHEN** patch JSON contains a parameter name that no longer exists in the initialized application
- **THEN** load ignores that saved parameter value
- **AND** continues loading values for other matching parameter names

#### Scenario: Missing saved value keeps default
- **WHEN** an initialized parameter has no saved value in patch JSON
- **THEN** that parameter keeps the value established by initialization

### Requirement: spp-5 — Miniapp consumes library persistence
WHEN the synth miniapp uses persistence, THE synth patch persistence system SHALL let the miniapp initialize its modules, parameters, modulation assignments, and MIDI setup through ordinary code first, then save/load parameter values through library patch persistence APIs without requiring a new UI surface; MIDI and audio setup SHALL be saved and loaded through the separate runtime configuration document.

#### Scenario: Code-defined miniapp patch can be saved and reloaded
- **WHEN** the miniapp default VCO code-defined initialization is run, edited, saved to patch JSON, initialized fresh, and loaded from that JSON
- **THEN** matching parameter names receive the saved parameter values
- **AND** MIDI input/output setup and audio device setup are not changed by loading the patch

#### Scenario: Missing saved patch uses defaults
- **WHEN** the miniapp starts and no patch file exists
- **THEN** it initializes the default parameter values without reporting a patch persistence failure

### Requirement: spp-6 — Patch lifecycle manager
WHEN synth patch lifecycle operations are requested, THE synth patch persistence system SHALL provide a JUCE-free library `PatchManager` that tracks the current patch directory as nullable state, orchestrates new/save/save-as/load/revert commands, and delegates state mutation/serialization through patch messages rather than app-specific persistence code.

#### Scenario: Current patch directory is nullable
- **WHEN** no patch has been loaded or saved-as
- **THEN** the patch manager reports no current patch directory
- **AND** save reports that a save-as path is required

#### Scenario: New patch clears current directory after reset
- **WHEN** new patch is requested
- **THEN** the patch manager dispatches a patch message requesting all values revert to defaults
- **AND** clears the current patch directory

#### Scenario: Save writes a new version for current patch
- **WHEN** the patch manager has a current patch directory
- **AND** save is requested
- **THEN** it dispatches a patch message requesting JSON serialization
- **AND** returns a pending status
- **AND** when `ProcessResponses` receives a serialized JSON response with the matching request id, it writes a new JSON version file in that directory without overwriting earlier versions

#### Scenario: Save is busy while serialization is pending
- **WHEN** save or save-as has already dispatched a serialization request that has not completed
- **THEN** another save or save-as request returns busy
- **AND** does not dispatch another serialization request

#### Scenario: Save-as requires a new directory
- **WHEN** save-as is requested with a directory path that does not exist
- **THEN** the patch manager dispatches a serialization request and returns pending
- **AND** when a matching serialized JSON response is received, it creates the directory, writes the first JSON version file there, and records that directory as the current patch
- **WHEN** save-as is requested with a target path that already exists as a file or directory
- **THEN** the patch manager rejects the request without overwriting existing patch history

#### Scenario: Load accepts directory or version path
- **WHEN** load is requested with a patch directory
- **THEN** the patch manager loads the latest sortable JSON version in that directory
- **AND** records that directory as the current patch
- **WHEN** load is requested with an explicit JSON version file
- **THEN** the patch manager loads that file
- **AND** records the containing directory as the current patch

#### Scenario: Load failure preserves current patch
- **WHEN** load is requested with a missing path, empty patch directory, corrupt JSON, unsupported patch schema, or a full patch-message queue
- **THEN** load reports failure
- **AND** the patch manager leaves its current patch directory unchanged

#### Scenario: Revert patch reloads current latest or defaults
- **WHEN** revert patch is requested and a current patch directory exists
- **THEN** the patch manager loads and dispatches the latest JSON version from that directory
- **AND** if the latest version cannot be read, parsed, validated, or dispatched, the current patch directory is left unchanged
- **WHEN** revert patch is requested and no current patch directory exists
- **THEN** it behaves like new patch by dispatching a full value reset and leaving the current directory unset

### Requirement: spp-7 — Patch message input/output buses
WHEN patch lifecycle operations interact with initialized synth state, THE synth patch persistence system SHALL provide patch-specific input messages for `LoadFromJSON`, `RevertAllToDefault`, and `SerializeToJSON`, and `MessageOut`/`MessageOutBus` types that can carry serialized JSON responses back to the patch manager.

#### Scenario: Load-from-JSON message carries a deserialized root
- **WHEN** a patch JSON version has been read and parsed successfully
- **THEN** the patch manager dispatches a `LoadFromJSON` message containing the already-deserialized JSON root
- **AND** the message retains ownership of the JSON arena until the message is consumed

#### Scenario: Serialize request produces JSON response
- **WHEN** initialized synth state receives a `SerializeToJSON` patch message
- **THEN** it serializes current parameter values to a patch JSON object
- **AND** posts a `MessageOut` response containing the JSON object and the serialize request's monotonic `requestId`
- **AND** the response retains ownership of the JSON arena until the patch manager consumes it

#### Scenario: Patch buses are command queues
- **WHEN** patch input or output messages are queued
- **THEN** consumers drain them explicitly in control/app code
- **AND** the patch queues do not gate messages by musical timestamp

#### Scenario: Revert message does not alter topology or runtime configuration
- **WHEN** initialized synth state receives a `RevertAllToDefault` patch message
- **THEN** it restores initialized parameter values to defaults
- **AND** it does not create or remove parameters, banks, pages, slots, modulation sources, MIDI mappings, profile definitions, MIDI instrument configuration, or audio device selection

#### Scenario: Library helper applies patch messages
- **WHEN** app code owns a `ParameterManager` and `MessageOutBus`
- **THEN** library code provides a helper to apply `LoadFromJSON`, `RevertAllToDefault`, and `SerializeToJSON` messages to those objects
- **AND** app-specific work after a successful load is limited to side effects caused by parameter changes

### Requirement: spp-8 — Miniapp patch manager integration
WHEN the synth miniapp is hosted by the synth application runtime, THE runtime SHALL instantiate the library patch manager and patch message buses, consume patch lifecycle messages after the application's ordinary initialization, and use the runtime-owned persistent patches directory for save-as/load operations.

#### Scenario: Miniapp save-as writes under runtime patches root
- **WHEN** the miniapp requests save-as through the runtime File page
- **THEN** it creates or selects a patch directory under the runtime-owned `patches/` directory
- **AND** the JSON version file is written by library patch manager/file helpers

#### Scenario: Miniapp load/revert route through patch messages
- **WHEN** the miniapp loads or reverts a patch
- **THEN** patch JSON load is delivered through `LoadFromJSON` or `RevertAllToDefault` patch messages
- **AND** MIDI instrument configuration and audio device selection are left unchanged by the patch message

### Requirement: spp-9 — Runtime configuration document
WHEN synth runtime configuration is saved, THE synth persistence system SHALL provide JUCE-free helpers that write a JSON object containing a runtime configuration schema identifier, schema version, MIDI instrument/controller configuration, and audio device selection state to an explicit configuration file path, and SHALL load that document into scratch state before mutating the caller's live configuration.

#### Scenario: Runtime configuration root has required sections
- **WHEN** runtime configuration is serialized
- **THEN** the JSON root contains `schema`, `schemaVersion`, `midiInstrument`, and `audioDevice`
- **AND** it does not contain patch parameter values or patch identity

#### Scenario: Invalid configuration leaves live state unchanged
- **WHEN** a runtime configuration document is missing required sections, has an unsupported schema, contains invalid MIDI instrument JSON, or contains invalid audio device JSON
- **THEN** loading fails
- **AND** the caller's live MIDI instrument and audio device state are unchanged

#### Scenario: Configuration save is atomic
- **WHEN** runtime configuration is saved to disk
- **THEN** the helper writes through a temporary file and renames it into place
- **AND** a failed write leaves the previous configuration file intact when one existed

### Requirement: spp-10 — Runtime data: Sheaf Patch launch configuration and app patch roots
WHEN a synth app is launched from the Sheaf Patch superapp, THE synth persistence system SHALL use the shared Sheaf Patch launch configuration path `<sheaf-user-data-root>/synth/sheaf-patch/config` as a JSON configuration file for MIDI/audio configuration and SHALL use `<sheaf-user-data-root>/synth/sheaf-patch/patches/<stable-app-id>` as the runtime-owned patches root for that selected app, where `<stable-app-id>` is the manifest app id from sar-19 and `<sheaf-user-data-root>` is the stable host user application data root used by standalone synth apps; standalone apps launched outside Sheaf Patch SHALL keep their existing default runtime data path behavior.

#### Scenario: Shared config path
- **WHEN** the miniapp is launched from Sheaf Patch and runtime configuration is saved
- **THEN** the configuration document is written to `<sheaf-user-data-root>/synth/sheaf-patch/config`
- **AND** the document contains runtime configuration only, not patch parameter values

#### Scenario: Per-app patch root
- **WHEN** the miniapp is launched from Sheaf Patch and a patch is saved-as
- **THEN** the patch directory is created below `<sheaf-user-data-root>/synth/sheaf-patch/patches/miniapp`
- **AND** no patch version file is written directly under another app's patch root

#### Scenario: Startup loads only selected app patches
- **WHEN** a selected app starts from Sheaf Patch
- **THEN** startup patch discovery searches only that app's `<sheaf-user-data-root>/synth/sheaf-patch/patches/<stable-app-id>` root
- **AND** patches saved for other registered apps are ignored

#### Scenario: Runtime data paths can split config and patches
- **WHEN** the runtime is supplied Sheaf Patch data paths
- **THEN** it accepts a config path and patches root that are not both derived from the selected app's display name
- **AND** patch lifecycle operations continue to use the supplied patches root

#### Scenario: Sheaf Patch logs root is supplied
- **WHEN** the runtime is supplied Sheaf Patch data paths
- **THEN** the supplied paths include a logs root at `<sheaf-user-data-root>/synth/sheaf-patch/logs`
- **AND** runtime startup uses that log root for apps launched by Sheaf Patch

#### Scenario: Standalone app paths are unchanged
- **WHEN** the miniapp is launched through its standalone executable rather than through Sheaf Patch
- **THEN** runtime configuration and patches continue to use the standalone app's default runtime data paths
- **AND** the standalone launch does not write configuration to `<sheaf-user-data-root>/synth/sheaf-patch/config`

### Requirement: spp-14 — Patch output bus: the patch manager is the only serialize requester
WHEN a host needs a serialized snapshot of the live patch for itself, THE patch manager SHALL request it on the patch input bus with a request id from its own counter, SHALL write no file for it, and SHALL hand the snapshot document to the consumer the host installed inside the same response-processing call that pops it, before any further serialize request is pushed. THE patch manager SHALL keep at most one serialize request outstanding. WHEN a save, save-as or save-as-overwrite is requested while a snapshot is outstanding, THE patch manager SHALL hold it in one slot, SHALL answer it as pending rather than busy, and SHALL dispatch it in the call that consumes the snapshot, after the consumer returns; WHEN a snapshot is requested while a save is outstanding or held, THE patch manager SHALL refuse it as busy and push nothing. A new-patch or load command SHALL drop a held save exactly as it drops an outstanding one.

#### Scenario: A save asked for during a snapshot is held, then written
- **WHEN** a host snapshot is outstanding and a save with a current patch directory is requested
- **THEN** the save answers pending, not busy
- **AND** after the snapshot is serialized and consumed, the held save is dispatched, and after its own serialization the save reports written with a new version file in that directory
- Check: not yet delivered: NEW host_snapshot_holds_a_save_requested_while_it_is_outstanding in tests/engine_tests.cpp (frogg3rs task S2)

#### Scenario: The consumer receives the live values and no file is written
- **WHEN** a host snapshot is requested and served
- **THEN** the consumer is called once with a document whose parameter values equal the engine's
- **AND** no file under the patches root changes
- Check: not yet delivered: NEW host_snapshot_reaches_the_consumer_and_writes_no_file in tests/engine_tests.cpp (frogg3rs task S2)

#### Scenario: A snapshot is refused while a save is outstanding
- **WHEN** a save is outstanding and a host snapshot is requested
- **THEN** the request answers busy and the patch input bus holds no new message
- Check: not yet delivered: NEW host_snapshot_is_refused_while_a_save_is_outstanding in tests/engine_tests.cpp (frogg3rs task S2)

### Requirement: spp-15 — Patch lifecycle: a host names the current patch without loading it
WHEN a host names a patch by a path relative to the engine's patches root, THE engine SHALL record the directory it names as the current patch directory without pushing any patch message, only when that path is not absolute, has no `..` component, and resolves, after its trailing separator is removed and symbolic links are followed, to an existing directory strictly inside the patches root; OTHERWISE, and when the host names no patch, THE engine SHALL leave no current patch directory. THE engine SHALL report the current patch directory back as a path relative to the patches root.

#### Scenario: A restored current patch takes the next save
- **WHEN** a host names `p` and then a save is requested
- **THEN** the naming pushed no patch message, the current patch reads back as `p`, and the save writes its version in the patches root's `p` directory
- Check: not yet delivered: NEW naming_an_existing_patch_makes_it_current_without_a_message in tests/engine_tests.cpp (frogg3rs task S3)

#### Scenario: A name that does not resolve inside the root is refused
- **WHEN** a host names a missing directory, a regular file, the root itself, `.`, a path containing `..`, an absolute path, or a symbolic link inside the root that points outside it
- **THEN** there is no current patch directory, and a save reports that a save-as path is required
- Check: not yet delivered: NEW naming_a_patch_outside_the_root_leaves_no_current_patch in tests/engine_tests.cpp (frogg3rs task S3)

