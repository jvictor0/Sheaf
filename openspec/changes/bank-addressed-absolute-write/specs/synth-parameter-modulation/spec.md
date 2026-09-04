# Delta — `synth-parameter-modulation`

**Added 2026-08-20** (the only absolute-write entry point into a bank is
slot-addressed and always lands on whatever the shared `BankSlot` currently
has selected and showing; this adds a write addressed to a bank instead of
to the current selection).

## ADDED Requirements

### Requirement: spm-90 — Messaging: bank-addressed absolute write
WHEN a `MessageIn::ParamSetAbsoluteOnBank(timestamp, bankIx, slotIx, position, normalizedValue, absoluteEpoch)` reaches `MessageInBus`, THE synth parameter modulation system SHALL resolve `position` to a physical encoder through the slot at `slotIx` exactly as the slot-addressed path does, SHALL apply the resulting absolute float target to the top-level parameter at that physical position on the bank at `bankIx` — independent of that bank's current visible/modulation-depth page — SHALL apply the edit regardless of any effective modifier, SHALL leave the addressed slot's selected bank unchanged, SHALL record `absoluteEpoch` on the addressed slot's position exactly as the slot-addressed path does, and SHALL leave state unchanged when the slot, the resolved position, the bank, or the top-level cell's parameter is not mapped.

#### Scenario: Bank-addressed write reaches the top-level parameter while modulation is showing
- **WHEN** a bank's modulation-depth view is open for one of its parameters
- **AND** a bank-addressed absolute message addresses that bank and the parameter's top-level position
- **THEN** the top-level parameter handles the absolute float target
- **AND** the open modulation-depth view is unaffected

#### Scenario: Bank-addressed write does not move the slot's selection
- **WHEN** a `BankSlot` has a bank selected other than the one a message addresses
- **AND** the bus applies a bank-addressed absolute message for the non-selected bank
- **THEN** the addressed bank's top-level parameter receives the write
- **AND** the slot's selected bank is unchanged

#### Scenario: A held modifier does not block a bank-addressed write
- **WHEN** any effective modifier is active
- **AND** the bus applies a mapped bank-addressed absolute message
- **THEN** the addressed parameter still receives the absolute edit

#### Scenario: Bank-addressed write still records its epoch
- **WHEN** a bank-addressed absolute message carries a non-zero `absoluteEpoch`
- **THEN** the addressed slot's position records that epoch exactly as the slot-addressed path does

#### Scenario: Unmapped bank-addressed address is a no-op
- **WHEN** the message addresses an absent slot, an out-of-range position, an absent bank, or a top-level position without a parameter
- **THEN** parameter state remains unchanged
