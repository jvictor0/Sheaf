# synth-controller-wizards Specification

Project: `projects/synth`. ID prefix: `scw`.

## Purpose

Define the controller-configuration wizard system: the JUCE-free
polymorphic wizard and portable config-form contract, the baked registry
of input/output pair matchers that classifies present MIDI devices into
available candidates, the MIDI Fighter Twister wizard's form and profile
generation, and the submit/ignore/reconfigure lifecycle that commits
generated profiles into the instrument configuration.
## Requirements
### Requirement: scw-1 — Architecture: typed controller wizard and portable config form
WHEN a controller-specific configuration wizard is implemented, THE synth controller-wizard system SHALL provide a JUCE-free polymorphic wizard contract that creates a wizard-specific in-memory configuration form and generates a complete controller profile from that form, SHALL expose each form through the shared portable `ui::Surface` tree/action contract, SHALL keep form ownership explicit, and SHALL ensure that a wizard can generate a profile only from the concrete form type it created.

#### Scenario: Concrete wizard creates its concrete form
- **WHEN** the MF Twister wizard creates a configuration form
- **THEN** the caller owns it through the abstract config-form contract
- **AND** the wizard retains a checked typed generation path for the concrete MF Twister form

#### Scenario: Form is portable and side-effect free
- **WHEN** a config form builds its tree or handles a field action
- **THEN** it reads or mutates only its in-memory form state
- **AND** it does not open MIDI endpoints, edit the engine, save runtime configuration, or call JUCE, DOM, or Web MIDI APIs

#### Scenario: Wrong form type cannot mutate configuration
- **WHEN** a wizard is asked to generate from a form created by a different wizard
- **THEN** generation reports a type-mismatch error
- **AND** no controller profile or instrument configuration is changed

### Requirement: scw-2 — Discovery: baked pair registry and candidate classification
WHEN present MIDI devices are classified for controller setup, THE synth controller-wizard system SHALL use a baked ordered registry of input/output pair matchers and wizard factories, SHALL produce deterministic candidates containing the concrete input and output endpoint identities, SHALL assign each present endpoint to at most one candidate, SHALL classify a candidate as available only when neither endpoint is claimed by an active or blacklisted instrument record, and SHALL retain unmatched present endpoint names as diagnostic data. The MF Twister descriptor SHALL recognize an input or output name by case-insensitive exact comparison with its descriptor-local alias lists, or, when the name ends with a trailing Android MIDI port suffix (" Output Port N" or " Input Port N", N one or more ASCII digits, case-insensitive), by that same comparison against the name with the suffix removed; no other prefix, substring, fuzzy, or implicit-number-suffix matching SHALL qualify a device.

<!-- RESTATES-EXCEPT
it adds an unlisted prefix, suffix, or other characters
  keeps: it adds an unlisted prefix
-->

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
- **BUT WHEN** it adds an unlisted prefix, or other characters, and carries no trailing Android port suffix
- **THEN** it is not eligible until that exact name is added as an alias
- **AND** its present name remains available in unmatched-device diagnostics

#### Scenario: An Android-reported port name is eligible once its port suffix is stripped
- **WHEN** a present endpoint name is one of the descriptor's aliases followed by a single space and then a trailing " Output Port N" or " Input Port N"
- **THEN** it is eligible for that descriptor's pairing the same as an exact match would be
- **AND** a name that is not exactly an alias plus that suffix pattern remains ineligible, whatever prefix, substring, or implicit-number variant it adds
- Check: `projects/synth/tests/controller_wizard_tests.cpp`, `DiscoveryMatchesAndroidUnnamedPortDeviceAfterStrippingItsPortSuffix` and `DiscoveryRejectsAndroidUnnamedPortDeviceWithAnUnrelatedName`; `DiscoveryRejectsPrefixSuffixAndImplicitNumberVariants` still passes.

#### Scenario: Half-configured record prevents contention
- **WHEN** an existing record claims only the input or only the output of a recognized pair
- **THEN** discovery does not offer the pair
- **AND** wizard submission cannot create a second record contending for the claimed endpoint

### Requirement: scw-3 — MF Twister: one encoder slot and exactly six buttons
WHEN the baked registry recognizes a MIDI Fighter Twister pair, THE synth controller-wizard system SHALL provide an MF Twister config form with one controller-wide Encoder Slot control followed by exactly six message-dropdown and argument-control pairs arranged as two columns of three buttons. The first column SHALL map buttons 0 through 2 to zero-based channel 3 CCs 8 through 10, and the second SHALL map buttons 3 through 5 to CCs 11 through 13. The Encoder Slot SHALL default to 0; all sixteen encoder positions' generated turn, push, and output mappings and every generated bank-selection or bank-navigation message SHALL target that slot. Buttons 0 through 5 SHALL default respectively to Hold Reset, Hold Random, Hold Random Mod, Next Bank, Start, and Previous Bank. Generation SHALL use the discovered input/output references, the existing sixteen-encoder-position defaults, exactly six side-button associations, and no side-button output feedback. This wizard-specific argument table SHALL override sru-30's low-level mapping-row presentation for Next/Previous Bank and SHALL NOT derive wizard argument enablement directly from `UISystemMessageHasArg`.

#### Scenario: Form has one slot and the six defaults
- **WHEN** a new MF Twister form opens
- **THEN** Encoder Slot appears once with value `0`
- **AND** buttons 0 through 2 appear in the first column and buttons 3 through 5 in the second
- **AND** their dropdowns show Hold Reset, Hold Random, Hold Random Mod, Next Bank, Start, and Previous Bank in order

#### Scenario: Supported choices are closed and every button is assigned
- **WHEN** any of the six message dropdowns opens
- **THEN** its choices are exactly Toggle Reset, Hold Reset, Toggle Random, Hold Random, Toggle Random Mod, Hold Random Mod, Toggle Gesture Select, Hold Gesture Select, Bank Select, Next Bank, Previous Bank, Start, Continue, Stop, Clock, and Scene Select
- **AND** no None, Unassigned, Gesture Value, Scene Blend, parameter-edit, or other catalog choice is offered

#### Scenario: Hold modifiers generate release messages
- **WHEN** the default form is submitted
- **THEN** Reset, Random, and Random Mod generate true-valued press messages and matching false-valued release messages
- **AND** the three mappings occupy side buttons 0, 1, and 2

#### Scenario: One encoder slot controls encoders and relative bank navigation
- **WHEN** the user sets Encoder Slot to `4` and submits a form containing Next Bank or Previous Bank
- **THEN** all sixteen encoder positions' generated turn, push, and output mappings target slot 4
- **AND** every generated Next Bank and Previous Bank message carries `slotIx = 4`
- **AND** neither relative-bank choice enables or consumes a per-button argument

#### Scenario: Bank Select retains a bank argument
- **WHEN** the user sets Encoder Slot to `4`, selects Bank Select for a button, enters argument `7`, and submits
- **THEN** the generated bank-selection message carries `slotIx = 4` and `bankIx = 7`

#### Scenario: Argument enablement follows message semantics
- **WHEN** a button selects Toggle Gesture Select, Hold Gesture Select, Bank Select, or Scene Select
- **THEN** its argument editor is visibly enabled and supplies respectively a gesture, bank, or scene index
- **BUT WHEN** a button selects any other supported choice, including Next Bank, Previous Bank, or Start
- **THEN** its paired argument control is visibly disabled
- **AND** its stored numeric value does not affect the generated message

#### Scenario: Generated profile uses Twister hardware addresses
- **WHEN** a valid six-button form is generated
- **THEN** it contains turn, push, and output mappings for all sixteen default encoder positions
- **AND** its six side-button associations use channel 3 CCs 8 through 13 with side-button output feedback disabled

#### Scenario: Numeric text uses the portable index domain
- **WHEN** Encoder Slot or an enabled button argument contains empty, negative, non-base-10, or overflowing text
- **THEN** validation identifies that field and refuses generation
- **AND** every non-negative base-10 integer representable by `std::size_t` is accepted

#### Scenario: Invalid form is atomic
- **WHEN** a form contains a non-enumerated message id or invalid numeric text
- **THEN** submission reports the affected field
- **AND** no profile is installed and the entered form state remains available for correction

### Requirement: scw-4 — Lifecycle: submit, ignore, and reconfigure
WHEN a wizard candidate or wizard-associated controller record is acted upon, THE synth controller-wizard system SHALL revalidate the target before committing, SHALL install valid generated profiles through one host-provided instrument commit, SHALL persist the registry descriptor's stable wizard id, SHALL persist Ignore as a blacklisted record with no runtime-active profile, SHALL preserve the existing name, endpoints, wizard id, and ordered position when reconfiguring a stored record, and SHALL request runtime-configuration save only after a Submit or Ignore commit succeeds. New records SHALL use the descriptor display name unless occupied, in which case they SHALL use the smallest free suffix beginning with ` 2`.

#### Scenario: New candidate installs atomically
- **WHEN** a present unclaimed candidate's valid form is submitted
- **THEN** one active controller record with both endpoint references, the descriptor wizard id, and the generated profile is committed
- **AND** the candidate ceases to be available
- **AND** runtime-configuration save is requested after the commit

#### Scenario: Generated names are deterministic
- **WHEN** `MIDI Fighter Twister` and `MIDI Fighter Twister 2` are already in use
- **THEN** submitting or ignoring a new MF Twister candidate names its record `MIDI Fighter Twister 3`

#### Scenario: Disappeared candidate refuses submit
- **WHEN** either endpoint of a new candidate disappears after its form opens and before Submit
- **THEN** submission reports that the controller must reconnect
- **AND** the form data and instrument configuration remain unchanged

#### Scenario: Ignore persists an inert record
- **WHEN** the user ignores an available candidate
- **THEN** one blacklisted record with the pair's hardware kind, stable wizard id, and endpoint identities is committed without an active profile
- **AND** the pair ceases to be available
- **AND** runtime-configuration save is requested after the commit

#### Scenario: Refused action does not save
- **WHEN** Submit or Ignore is refused before the instrument commit
- **THEN** runtime-configuration save is not requested
- **AND** the prior persisted configuration remains authoritative

#### Scenario: Reconfigure preserves record identity
- **WHEN** an existing wizard-associated active or blacklisted record submits a valid form
- **THEN** its generated profile and Active disposition replace the prior profile/disposition
- **AND** its name, input/output references, wizard id, and ordered position are preserved

#### Scenario: Reconfigure replaces the complete profile
- **WHEN** a wizard-associated Twister profile is not exactly the complete generated shape—no analog or extra mappings; exactly the default sixteen turn, sixteen push, and sixteen output mappings; exactly six expressible system associations at channel 3 CCs 8 through 13; and one common slot across every encoder and bank message
- **THEN** the reconfiguration form opens with wizard defaults and warns that Submit replaces the whole profile
- **AND** submitting drops extra or hand-edited mappings rather than merging them

#### Scenario: Compatible Twister profile seeds one slot
- **WHEN** a wizard-associated Twister profile has exactly the complete generated mapping shape and every encoder and bank message coherently targets slot 4
- **THEN** Reconfigure seeds Encoder Slot with `4` and seeds the six expressible button choices from that profile

#### Scenario: Offline existing record can be reconfigured
- **WHEN** an existing wizard-associated record's stored devices are absent
- **THEN** its form SHALL open from the stored kind and available profile data
- **AND** valid generation and commit SHALL NOT require the endpoints to be present

