# Delta — `synth-controller-wizards`

## MODIFIED Requirements

### Requirement: scw-2 — Discovery: baked pair registry and candidate classification
WHEN present MIDI devices are classified for controller setup, THE synth controller-wizard system SHALL classify them against a registry built by `MakeControllerWizardRegistry(catalog)`: one descriptor per app-supplied device default when the catalog's device-default list is non-empty, else the library's single MF Twister descriptor. THE synth controller-wizard system SHALL produce deterministic candidates containing the concrete input and output endpoint identities, SHALL assign each present endpoint to at most one candidate, SHALL classify a candidate as available only when neither endpoint is claimed by an active or blacklisted instrument record, and SHALL retain unmatched present endpoint names as diagnostic data. Discovery SHALL walk the registry in declaration order and classify a present device pair under the first descriptor whose input and output alias lists both match; every descriptor, library or app-supplied, SHALL recognize input and output names only by case-insensitive exact comparison with its own alias lists — prefix, substring, fuzzy, and implicit-number-suffix matching SHALL NOT qualify a device.

#### Scenario: Recognized unclaimed pair is available
- **WHEN** a registry-recognized input and output are both present and neither is referenced by any instrument record
- **THEN** discovery returns one available candidate carrying both identifiers and names
- **AND** the available-candidate warning state is true
- Check: none in tree (inherited scenario)

#### Scenario: Active profile suppresses discovery
- **WHEN** an active controller record claims either endpoint of an otherwise recognized pair
- **THEN** discovery does not offer that pair as an available candidate
- Check: `controller_wizard_tests.cpp: DiscoverySuppressesPairsClaimedByActiveAndBlacklistedRecords`

#### Scenario: Blacklist suppresses discovery
- **WHEN** a blacklisted controller record claims a recognized pair
- **THEN** discovery does not offer that pair as an available candidate
- **AND** the pair does not contribute to the warning state
- Check: none in tree (inherited scenario)

#### Scenario: Pairing is deterministic and exclusive
- **WHEN** discovery receives the same ordered device lists and instrument snapshot twice
- **THEN** it returns candidates in the same order with the same pairings
- **AND** no input or output identity occurs in more than one candidate
- Check: `controller_wizard_tests.cpp: DiscoveryResultsAreStableAndInputsRemainUnchanged`

#### Scenario: Exact alias matching is required
- **WHEN** a present endpoint name differs from every registry alias only by case
- **THEN** it is eligible for pairing
- **BUT WHEN** it adds an unlisted prefix, suffix, or other characters
- **THEN** it is not eligible until that exact name is added as an alias
- **AND** its present name remains available in unmatched-device diagnostics
- Check: `controller_wizard_tests.cpp: DiscoveryRejectsPrefixSuffixAndImplicitNumberVariants`

#### Scenario: Half-configured record prevents contention
- **WHEN** an existing record claims only the input or only the output of a recognized pair
- **THEN** discovery does not offer the pair
- **AND** wizard submission cannot create a second record contending for the claimed endpoint
- Check: `controller_wizard_tests.cpp: DiscoveryTreatsHalfConfiguredStoredRefsAsEndpointClaims`

#### Scenario: Empty catalog registry is Twister-only
- **WHEN** `MakeControllerWizardRegistry` is called with a catalog whose device-default list is empty
- **THEN** the returned registry contains exactly the library's MF Twister descriptor
- Check: `controller_wizard_tests.cpp: MakeControllerWizardRegistryWithEmptyCatalogReturnsTheOneTwisterDescriptor`

#### Scenario: App-catalog registry is one descriptor per default, no library descriptor mixed in
- **WHEN** `MakeControllerWizardRegistry` is called with a catalog whose device-default list is non-empty
- **THEN** the returned registry contains exactly one descriptor per device default, in the catalog's order, and no MF Twister descriptor
- Check: `controller_wizard_tests.cpp: MakeControllerWizardRegistryWithAppDefaultsReturnsOneDescriptorPerDefault`

#### Scenario: First matching descriptor wins when defaults share aliases
- **WHEN** two app device defaults' alias lists both match a present device pair
- **THEN** discovery classifies the pair under whichever descriptor appears first in the registry
- Check: `controller_wizard_tests.cpp: DiscoveryWithAppRegistryClassifiesDeviceByFirstDefaultsInputAlias`

### Requirement: scw-4 — Lifecycle: submit, ignore, and blacklisted-record reconfigure
WHEN a wizard candidate or a Blacklisted wizard-associated controller record is acted upon, THE synth controller-wizard system SHALL revalidate the target before committing, SHALL install valid generated profiles through one host-provided instrument commit, SHALL persist the registry descriptor's stable wizard id, SHALL persist Ignore as a blacklisted record with no runtime-active profile, SHALL request runtime-configuration save only after a Submit or Ignore commit succeeds, and, when a form opened from an existing Blacklisted wizard-associated record is submitted, SHALL preserve that record's name, endpoints, wizard id, and ordered position while replacing its profile and Active disposition. New records SHALL use the descriptor display name unless occupied, in which case they SHALL use the smallest free suffix beginning with ` 2`. An Active wizard-associated record's stored profile is regenerated only through the runtime UI's Restore action (`synth-runtime-ui`, sru-60), never through a wizard form opened for that purpose; this lifecycle offers no Active-record reconfigure entry point.

#### Scenario: New candidate installs atomically
- **WHEN** a present unclaimed candidate's valid form is submitted
- **THEN** one active controller record with both endpoint references, the descriptor wizard id, and the generated profile is committed
- **AND** the candidate ceases to be available
- **AND** runtime-configuration save is requested after the commit
- Check: `controllers_page_ui_tests.cpp: TestWizardSubmitCommitsCompleteProfileThenSaves`

#### Scenario: Generated names are deterministic
- **WHEN** `MIDI Fighter Twister` and `MIDI Fighter Twister 2` are already in use
- **THEN** submitting or ignoring a new MF Twister candidate names its record `MIDI Fighter Twister 3`
- Check: `controller_wizard_tests.cpp: MfTwisterWizardGeneratesCompleteActiveProfileFromItsForm`

#### Scenario: Disappeared candidate refuses submit
- **WHEN** either endpoint of a new candidate disappears after its form opens and before Submit
- **THEN** submission reports that the controller must reconnect
- **AND** the form data and instrument configuration remain unchanged
- Check: `controllers_page_ui_tests.cpp: TestWizardSubmitRefusalsRetainFormAndPersistence`

#### Scenario: Ignore persists an inert record
- **WHEN** the user ignores an available candidate
- **THEN** one blacklisted record with the pair's hardware kind, stable wizard id, and endpoint identities is committed without an active profile
- **AND** the pair ceases to be available
- **AND** runtime-configuration save is requested after the commit
- Check: `controllers_page_ui_tests.cpp: TestWizardIgnoreCommitsOneInertBlacklistedRecord`

#### Scenario: Refused action does not save
- **WHEN** Submit or Ignore is refused before the instrument commit
- **THEN** runtime-configuration save is not requested
- **AND** the prior persisted configuration remains authoritative
- Check: `controllers_page_ui_tests.cpp: TestWizardSubmitRefusalsRetainFormAndPersistence`

#### Scenario: Blacklisted-record reconfigure preserves record identity
- **WHEN** an existing Blacklisted wizard-associated record submits a valid form opened from it
- **THEN** its generated profile and Active disposition replace the prior profile/disposition
- **AND** its name, input/output references, wizard id, and ordered position are preserved
- Check: `controllers_page_ui_tests.cpp: TestConfigureSeedsFromDormantDataForBlacklistedRecords`

#### Scenario: Blacklisted-record reconfigure replaces the complete profile
- **WHEN** a wizard-associated Twister profile is not exactly the complete generated shape—no analog or extra mappings; exactly the default sixteen turn, sixteen push, and sixteen output mappings; exactly six expressible system associations at channel 3 CCs 8 through 13; and one common slot across every encoder and bank message
- **THEN** the form opened from the Blacklisted record opens with wizard defaults and warns that Submit replaces the whole profile
- **AND** submitting drops extra or hand-edited mappings rather than merging them
- Check: `controllers_page_ui_tests.cpp: TestConfigureSeedsFromDormantDataForBlacklistedRecords`

#### Scenario: Compatible dormant Twister profile seeds one slot
- **WHEN** a Blacklisted wizard-associated record's retained profile has exactly the complete generated mapping shape and every encoder and bank message coherently targets slot 4
- **THEN** the form opened from that record seeds Encoder Slot with `4` and seeds the six expressible button choices from that profile, without a destructive-replace warning
- Check: `controllers_page_ui_tests.cpp: TestConfigureSeedsFromDormantDataForBlacklistedRecords`

#### Scenario: Offline Blacklisted record can be reconfigured
- **WHEN** an existing Blacklisted wizard-associated record's stored devices are absent
- **THEN** its form SHALL open from the stored kind and available profile data
- **AND** valid generation and commit SHALL NOT require the endpoints to be present
- Check: `controllers_page_ui_tests.cpp: TestConfigureSeedsFromDormantDataForBlacklistedRecords`

## ADDED Requirements

### Requirement: scw-5 — App default wizard: fixed-config, empty form
WHEN a controller wizard is created from an app device default's registry descriptor, THE synth controller-wizard system SHALL present an empty configuration form with no fields, SHALL treat that form as always valid, and SHALL generate a profile equal to the device default's own stored `MidiControllerProfileConfig`, with only the controller name, input reference, and output reference taken from the generation context.

#### Scenario: App default form is empty and always valid
- **WHEN** an app device default's config form is built
- **THEN** it contains no editable fields
- **AND** validating it always succeeds
- Check: `controller_wizard_tests.cpp: AppDefaultControllerWizardValidatesEmptyFormAndGeneratesTheStoredConfig`

#### Scenario: Generation reproduces the stored config with the context's identity
- **WHEN** an app device default's wizard generates a profile for a given name, input, and output
- **THEN** the resulting controller's config equals the device default's stored config exactly
- **AND** its name, input, and output equal the generation context's, and its kind and wizard id equal the device default's
- Check: `controller_wizard_tests.cpp: AppDefaultControllerWizardValidatesEmptyFormAndGeneratesTheStoredConfig`
