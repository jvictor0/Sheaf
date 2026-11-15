# Delta — `synth-patch-persistence`

## MODIFIED Requirements

### Requirement: spp-6 — Patch lifecycle manager
WHEN synth patch lifecycle operations are requested, THE synth patch persistence system SHALL provide a JUCE-free library `PatchManager` that tracks the current patch directory as nullable state, orchestrates new/save/save-as/load commands, and delegates state mutation/serialization through patch messages rather than app-specific persistence code.

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

Dropped scenario: **Revert patch reloads current latest or defaults**. Revert is removed; Load reloads a chosen patch's newest version and New performs the same full value reset this scenario's no-current-directory case described (`synth-runtime-ui` D8).

### Requirement: spp-8 — Miniapp patch manager integration
WHEN the synth miniapp is hosted by the synth application runtime, THE runtime SHALL instantiate the library patch manager and patch message buses, consume patch lifecycle messages after the application's ordinary initialization, and use the runtime-owned persistent patches directory for save-as/load operations.

#### Scenario: Miniapp save-as writes under runtime patches root
- **WHEN** the miniapp requests save-as through the runtime File page
- **THEN** it creates or selects a patch directory under the runtime-owned `patches/` directory
- **AND** the JSON version file is written by library patch manager/file helpers

#### Scenario: Miniapp load and new route through patch messages
- **WHEN** the miniapp loads a patch, or starts a new one
- **THEN** patch JSON load is delivered through `LoadFromJSON`, and a new patch's full reset through `RevertAllToDefault`, patch messages
- **AND** MIDI instrument configuration and audio device selection are left unchanged by the patch message

### Requirement: spp-9 — Runtime configuration document
WHEN synth runtime configuration is saved, THE synth persistence system SHALL provide JUCE-free helpers that write a JSON object containing a runtime configuration schema identifier, schema version, MIDI instrument/controller configuration, audio device selection state, and the patch version file last opened or saved (relative to the patches root, and empty after New) to an explicit configuration file path, and SHALL load that document into scratch state before mutating the caller's live configuration.

#### Scenario: Runtime configuration root has required sections
- **WHEN** runtime configuration is serialized
- **THEN** the JSON root contains `schema`, `schemaVersion`, `midiInstrument`, and `audioDevice`
- **AND** it carries the last opened or saved patch version once a patch has been opened or saved, and no patch parameter values
- **AND** a document written before this record existed still loads, with no record

#### Scenario: Invalid configuration leaves live state unchanged
- **WHEN** a runtime configuration document is missing required sections, has an unsupported schema, contains invalid MIDI instrument JSON, or contains invalid audio device JSON
- **THEN** loading fails
- **AND** the caller's live MIDI instrument and audio device state are unchanged

#### Scenario: Configuration save is atomic
- **WHEN** runtime configuration is saved to disk
- **THEN** the helper writes through a temporary file and renames it into place
- **AND** a failed write leaves the previous configuration file intact when one existed
