# Delta — `synth-controller-wizards`

## MODIFIED Requirements

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
