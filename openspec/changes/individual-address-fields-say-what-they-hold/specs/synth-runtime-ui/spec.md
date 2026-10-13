# Delta — `synth-runtime-ui`

sru-68 is added. It states, at the individual-row level, the same header
rule `block-end-fields-show-the-last-control`'s O1 ruling states for block
rows: one header per field, true on every row it heads; where a field's
meaning differs by address type, the row carries its own field.

Every scenario below is backed by a named test; none carries a "not yet
delivered" Check line.

## ADDED Requirements

### Requirement: sru-68 — Controllers page: an individual row's number field says what it holds
WHEN the Controllers page shows an individual (non-block) encoder push row or Generic system-message row, THE runtime UI SHALL head that row's address-number field "CC" while the row's own control is CC-addressed and "Note" while it is Note-addressed, through NEW `Field::Note` in place of `Field::Cc` on a Note-addressed row, and SHALL read and write the same stored control number under either field; an individual row with no Note/CC selector -- an encoder turn row, an analog gesture row, or an analog app-action row -- SHALL keep `Field::Cc` headed "CC" regardless of any other row's address type.

#### Scenario: An individual push row's number field follows its address type
- **WHEN** an individual encoder push row is switched from CC to Note
- **THEN** its number field is `Field::Note`, headed "Note", reading the
  same number the row showed as `Field::Cc`
- **AND** `Field::Cc` is no longer editable on that row
- Check: `viewmodel_tests.cpp` `AddressTypeIndividualEditRoundTripsAndRejectedIndicesPreserveSession`

#### Scenario: An individual Generic system row's number field follows its address type
- **WHEN** an individual Generic system-message row is switched from CC to Note
- **THEN** its number field is `Field::Note`, headed "Note", reading the
  same number the row showed as `Field::Cc`
- **AND** `Field::Cc` is no longer editable on that row
- Check: `viewmodel_tests.cpp` `AddressTypeIndividualEditRoundTripsAndRejectedIndicesPreserveSession`

#### Scenario: Switching back to CC restores the CC field
- **WHEN** a Note-addressed individual push row or Generic system row is
  switched back to CC
- **THEN** its number field is `Field::Cc` again, headed "CC"
- **AND** `Field::Note` is no longer editable on that row
- Check: `viewmodel_tests.cpp` `AddressTypeIndividualEditRoundTripsAndRejectedIndicesPreserveSession`

#### Scenario: An out-of-range number is refused by the name the page shows
- **WHEN** `128` is typed into a Note-addressed individual row's number field
- **THEN** the commit is refused with "note must be an integer 0-127"
- **AND** the open row is unchanged
- Check: `viewmodel_tests.cpp` `AddressTypeIndividualEditRoundTripsAndRejectedIndicesPreserveSession`

#### Scenario: Rows with no Note/CC selector stay headed CC
- **WHEN** an encoder turn row or an analog gesture row is shown
- **THEN** its number field is `Field::Cc`, headed "CC", regardless of any
  other row's address type
- Check: `viewmodel_tests.cpp` `RowFieldValueReadsEncoderTurnChannelCcSlotIxPosition`, `RowFieldValueReadsAnalogGestureFieldsAndSceneBlend`
