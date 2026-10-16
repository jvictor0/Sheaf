# Delta — `synth-patch-persistence`

## ADDED Requirements

### Requirement: spp-13 — Save: the serialization arena has no ceiling
WHEN a dispatched save's `SerializeToJSON` message exhausts the serialization arena, THE synth patch persistence system SHALL grow that arena and retry, on the message thread, with no upper bound on the arena's capacity, until the patch fits; the caller-owned engine arena grows through `MessageThreadTick`'s `GrowSerializationArenaForTick`, one doubling per tick, and a non-caller-owned `SerializeToJSON` call grows its own arena the same way, in place. A dispatched save's pending state SHALL be cleared only by a matching serialized response, never by a growth limit dropping the stashed message.

#### Scenario: A save of a large session completes
- **WHEN** a patch's live depth count is large enough that its serialized JSON does not fit the arena's starting capacity
- **THEN** each following `MessageThreadTick` doubles the arena until the patch fits
- **AND** the dispatched save completes and its pending state clears

#### Scenario: A save after a large save is not busy
- **WHEN** a save has just completed for a patch whose depths had exhausted and regrown the arena
- **THEN** the next save or save-as dispatches normally instead of reporting busy

#### Scenario: A DAW snapshot of a large session updates
- **WHEN** a plugin's session-state snapshot serializes through the same engine arena and depth count that would have required arena growth
- **THEN** the snapshot completes and `getStateInformation` returns the grown state, not a value cached from before the growth
