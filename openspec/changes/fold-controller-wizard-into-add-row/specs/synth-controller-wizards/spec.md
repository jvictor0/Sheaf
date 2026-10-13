# Delta — `synth-controller-wizards`

## Purpose

Define the controller-configuration wizard system: the JUCE-free
polymorphic wizard and portable config-form contract, the baked registry
of input/output pair matchers that classifies present MIDI devices into
available candidates, and the MIDI Fighter Twister wizard's form and
profile generation that the Controllers page's add row and Restore install
into the instrument configuration.

## MODIFIED Requirements

### Requirement: scw-1 — Architecture: typed controller wizard and portable config form
WHEN a controller-specific configuration wizard is implemented, THE synth controller-wizard system SHALL provide a JUCE-free polymorphic wizard contract that creates a wizard-specific in-memory configuration form and generates a complete controller profile from that form, SHALL keep form ownership explicit, and SHALL ensure that a wizard can generate a profile only from the concrete form type it created. A configuration form SHALL be generation input only: no runtime page renders it or dispatches actions to it.

#### Scenario: Concrete wizard creates its concrete form
- **WHEN** the MF Twister wizard creates a configuration form
- **THEN** the caller owns it through the abstract config-form contract
- **AND** the wizard retains a checked typed generation path for the concrete MF Twister form

#### Scenario: Form is side-effect free
- **WHEN** a config form is created or validated
- **THEN** it reads or mutates only its in-memory form state
- **AND** it does not open MIDI endpoints, edit the engine, save runtime configuration, or call JUCE, DOM, or Web MIDI APIs

#### Scenario: Wrong form type cannot mutate configuration
- **WHEN** a wizard is asked to generate from a form created by a different wizard
- **THEN** generation reports a type-mismatch error
- **AND** no controller profile or instrument configuration is changed

### Requirement: scw-2 — Discovery: baked pair registry and candidate classification
WHEN present MIDI devices are classified for controller setup, THE synth controller-wizard system SHALL use a baked ordered registry of input/output pair matchers and wizard factories, SHALL produce deterministic candidates containing the concrete input and output endpoint identities, SHALL assign each present endpoint to at most one candidate, SHALL classify a candidate as available only when neither endpoint is claimed by an active or blacklisted instrument record, and SHALL retain unmatched present endpoint names as diagnostic data. The MF Twister descriptor SHALL recognize input and output names only by case-insensitive exact comparison with its descriptor-local alias lists; prefix, substring, fuzzy, and implicit-number-suffix matching SHALL NOT qualify a device.

#### Scenario: Recognized unclaimed pair is available
- **WHEN** a registry-recognized input and output are both present and neither is referenced by any instrument record
- **THEN** discovery returns one available candidate carrying both identifiers and names
- **AND** the available-candidate warning state is true

#### Scenario: Active profile suppresses discovery
- **WHEN** an active controller record claims either endpoint of an otherwise recognized pair
- **THEN** discovery does not offer that pair as an available candidate

#### Scenario: Blacklist suppresses discovery
- **WHEN** a blacklisted controller record claims a recognized pair
- **THEN** discovery does not offer that pair as an available candidate
- **AND** the pair does not contribute to the warning state

#### Scenario: Pairing is deterministic and exclusive
- **WHEN** discovery receives the same ordered device lists and instrument snapshot twice
- **THEN** it returns candidates in the same order with the same pairings
- **AND** no input or output identity occurs in more than one candidate

#### Scenario: Exact alias matching is required
- **WHEN** a present endpoint name differs from every MF Twister alias only by case
- **THEN** it is eligible for MF Twister pairing
- **BUT WHEN** it adds an unlisted prefix, suffix, or other characters
- **THEN** it is not eligible until that exact name is added as an alias
- **AND** its present name remains available in unmatched-device diagnostics

#### Scenario: Half-configured record prevents contention
- **WHEN** an existing record claims only the input or only the output of a recognized pair
- **THEN** discovery does not offer the pair
- **AND** Add cannot create a second record contending for the claimed endpoint

### Requirement: scw-3 — MF Twister: one encoder slot and exactly six buttons
WHEN the library MIDI Fighter Twister wizard generates a profile, THE synth controller-wizard system SHALL generate it from a default form of one controller-wide Encoder Slot of 0 and exactly six side buttons, mapping buttons 0 through 2 to zero-based channel 3 CCs 8 through 10 and buttons 3 through 5 to CCs 11 through 13, with buttons 0 through 5 defaulting respectively to Hold Reset, Hold Random, Hold Random Mod, Next Bank, Start, and Previous Bank. All sixteen encoder positions' generated turn, push, and output mappings and every generated bank-selection or bank-navigation message SHALL target the Encoder Slot. Generation SHALL use the given input/output references, the existing sixteen-encoder-position defaults, exactly six side-button associations, and no side-button output feedback.

#### Scenario: The default form generates the six defaults
- **WHEN** the default MF Twister form generates a profile
- **THEN** its six side-button associations carry Hold Reset, Hold Random, Hold Random Mod, Next Bank, Start, and Previous Bank in order
- **AND** every encoder mapping and every generated Next Bank and Previous Bank message targets slot 0

#### Scenario: Hold modifiers generate release messages
- **WHEN** the default form generates a profile
- **THEN** Reset, Random, and Random Mod generate true-valued press messages and matching false-valued release messages
- **AND** the three mappings occupy side buttons 0, 1, and 2

#### Scenario: Generated profile uses Twister hardware addresses
- **WHEN** a valid six-button form is generated
- **THEN** it contains turn, push, and output mappings for all sixteen default encoder positions
- **AND** its six side-button associations use channel 3 CCs 8 through 13 with side-button output feedback disabled

### Requirement: scw-5 — App default wizard: fixed-config, empty form
WHEN a controller wizard is created from an app device default's registry descriptor, THE synth controller-wizard system SHALL create an empty configuration form with no fields, SHALL treat that form as always valid, and SHALL generate a profile equal to the device default's own stored `MidiControllerProfileConfig`, with only the controller name, input reference, and output reference taken from the generation context.

#### Scenario: App default form is empty and always valid
- **WHEN** an app device default's config form is created
- **THEN** it contains no fields
- **AND** validating it always succeeds
- Check: `controller_wizard_tests.cpp: AppDefaultControllerWizardValidatesEmptyFormAndGeneratesTheStoredConfig`

#### Scenario: Generation reproduces the stored config with the context's identity
- **WHEN** an app device default's wizard generates a profile for a given name, input, and output
- **THEN** the resulting controller's config equals the device default's stored config exactly
- **AND** its name, input, and output equal the generation context's, and its kind and wizard id equal the device default's
- Check: `controller_wizard_tests.cpp: AppDefaultControllerWizardValidatesEmptyFormAndGeneratesTheStoredConfig`

## REMOVED Requirements

### Requirement: scw-4 — Lifecycle: submit, ignore, and reconfigure
**Reason**: The wizard page, its Submit and Ignore, and the released row's Configure are removed. The add row installs a preset (`synth-runtime-ui` sru-4), and Restore regenerates one (sru-60). The naming rule this requirement carried (the display name, or the smallest free suffix beginning with ` 2`) now lives in sru-4.
**Migration**: Add from the add row. A released record loaded from an earlier configuration offers Delete.
