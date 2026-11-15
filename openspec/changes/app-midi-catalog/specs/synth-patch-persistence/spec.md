# Delta — `synth-patch-persistence`

## MODIFIED Requirements

### Requirement: spp-2 — Patch document format
WHEN a synth patch is saved, THE synth patch persistence system SHALL write a JSON object containing a synth patch schema identifier, schema version, patch name, and recursive parameter values keyed by initialized parameter name, while excluding audio device selection state, parameter definitions, and synth topology from persisted patch JSON; if legacy patch JSON contains a single-`midiProfile` or `audioDevice` section, patch validation SHALL tolerate those extra sections without applying them as patch state. A save requested without instrument-carrying SHALL write schema version 1 with no `midiInstrument` section, byte for byte what a parameter-only patch has always written; a save requested with instrument-carrying SHALL write schema version 2 and a `midiInstrument` section holding the live instrument, serialized with the existing instrument JSON helpers. Loading SHALL accept a root whose schema version is 1 or 2; a schema version outside that pair SHALL be rejected.

#### Scenario: Patch root has required sections
- **WHEN** a version-1 patch is serialized
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
- **WHEN** a patch document contains legacy `midiProfile` or `audioDevice` sections
- **THEN** patch validation succeeds if the patch schema and parameter values are otherwise valid
- **AND** loading the patch leaves current MIDI instrument configuration and audio device state unchanged

#### Scenario: Instrument-carrying save writes schema version 2 with the instrument
- **WHEN** a save is requested with instrument-carrying on
- **THEN** the written root's `schemaVersion` is 2
- **AND** it contains a `midiInstrument` section equal to the live instrument
- Check: `engine_tests.cpp: engine_patch_load_restores_saved_instrument_when_catalog_carries_mappings`

#### Scenario: A save without instrument-carrying is unchanged from before this capability existed
- **WHEN** a save is requested with instrument-carrying off
- **THEN** the written root's `schemaVersion` is 1 and it contains no `midiInstrument` key, identical to a parameter-only save
- Check: `engine_tests.cpp: engine_patch_load_leaves_instrument_untouched_when_catalog_does_not_carry_mappings`

#### Scenario: A schema-version-2 root loads
- **WHEN** a schema-version-2 patch document is loaded
- **THEN** validation succeeds and parameter values apply, the same as a version-1 document

### Requirement: spp-4 — Patch save and load APIs
WHEN application code requests synth patch save or load without a UI, THE synth patch persistence system SHALL expose JUCE-free library APIs to serialize initialized parameter values to JSON, parse patch JSON with a caller-owned arena, and apply only matching named parameter values to an already initialized parameter manager. `BuildPatchJSON` SHALL accept a `carryInstrument` flag selecting the spp-2 schema-version-1/2 behavior; `LoadPatchJSON` and `ApplyPatchMessage` SHALL accept an optional `loadedInstrument` out-parameter that, when non-null and the loaded root is schema version 2 with a `midiInstrument` section that parses, receives the parsed instrument, and is left unset in every other case (out-parameter null, schema version 1, section absent, section unparseable); THE library SHALL NOT apply a parsed instrument to any live state itself — a caller that wants it applied SHALL do so through its own instrument-edit path. Application code that forwards a running app's catalog SHALL pass `carryInstrument` as the catalog's `patchCarriesMappings` and, only when that is set, supply a `loadedInstrument` out-parameter and apply a value it receives on the message thread, through the same instrument-edit path used for a UI edit; the audio thread SHALL NOT write live instrument state from a loaded patch.

#### Scenario: Programmatic save returns JSON
- **WHEN** tests, reusable synth callers, or miniapp code request a patch save for an initialized synth instance
- **THEN** the persistence API returns or writes a JSON patch document without requiring a visible UI
- **AND** a patch document requested without instrument-carrying excludes MIDI instrument/controller configuration and audio device selection

#### Scenario: Programmatic load tolerates app changes
- **WHEN** patch JSON contains a parameter name that no longer exists in the initialized application
- **THEN** load ignores that saved parameter value
- **AND** continues loading values for other matching parameter names

#### Scenario: Missing saved value keeps default
- **WHEN** an initialized parameter has no saved value in patch JSON
- **THEN** that parameter keeps the value established by initialization

#### Scenario: A parsed instrument is handed back, never applied by the loader
- **WHEN** `LoadPatchJSON` is called with a non-null `loadedInstrument` on a schema-version-2 document with a valid `midiInstrument` section
- **THEN** the parsed instrument is written to `*loadedInstrument`
- **AND** the `instrument` argument passed to the call is not modified
- Check: `engine_tests.cpp: engine_patch_load_restores_saved_instrument_when_catalog_carries_mappings`

#### Scenario: An app whose catalog does not carry mappings never parses a present section
- **WHEN** a schema-version-2 file with a valid `midiInstrument` section is loaded by an app whose catalog's `patchCarriesMappings` is unset
- **THEN** the engine passes no `loadedInstrument` out-parameter
- **AND** the live instrument configuration is left exactly as it was before the load
- Check: `engine_tests.cpp: engine_ignores_version_two_midi_instrument_section_when_catalog_does_not_carry_mappings`

#### Scenario: A loaded instrument is applied later, on the message thread, through the instrument-edit path
- **WHEN** a patch carrying an instrument section is loaded by an app whose catalog carries mappings
- **THEN** the live instrument is unchanged immediately after the audio-thread drain that parsed it
- **AND** it is replaced with the loaded instrument only on the next message-thread tick, through the same edit path a UI commit uses
- Check: `engine_tests.cpp: engine_patch_load_restores_saved_instrument_when_catalog_carries_mappings`
