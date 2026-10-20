# Delta — `synth-patch-persistence`

A patch whose depths exceed available storage is provisioned before it ever
reaches the parameter authority: whichever caller parses the patch
document, on the message thread, adds storage to every group the patch's
depths would leave short, then pushes the load message. The engine's
existing arena-exhausted retry (inline before audio; stash and retry while
running) is unchanged and independent of this provisioning.

## ADDED Requirements

### Requirement: spp-12 — Load: a patch never applies with a depth missing
WHEN a caller on the message thread parses a patch document it is about to load, THE caller SHALL provision, for every parameter group the patch's depths would leave short of that group's low watermark, one storage batch sized at the missing depth count plus the watermark, before pushing the parsed message as a LoadFromJSON message; a group whose available storage already covers its own missing depths plus its watermark SHALL be left untouched. Applying the pushed message THEN never finds a group short of the storage its own depths need, at every site that applies patch messages: before audio, inline on the initializing thread and before the first block; and while running, on the first block that drains the message. The arena's own exhaustion retry SHALL stay as each site has it, independent of this provisioning.

#### Scenario: A depth-heavy patch opens whole at startup
- **WHEN** an engine initializes on data paths whose last-opened patch needs more depths than the launch batch leaves above the watermark
- **THEN** the patch is applied whole before the first audio block

#### Scenario: A depth-heavy patch loads whole while running
- **WHEN** a patch with more depths than available storage is loaded on a running engine
- **THEN** the running patch is unchanged up to the block that drains the load message, and every depth of the loaded patch is live once that block applies it

#### Scenario: The arena retry is unchanged
- **WHEN** the serialization arena is exhausted during a load
- **THEN** the existing stash-and-retry behaviour is observed unchanged

#### Scenario: A Load that fits leaves one press of storage behind it
- **WHEN** a patch that fits is loaded and a press that allocates depths follows in the same tick
- **THEN** the press finds at least the watermark available after the Load applies

#### Scenario: A DAW restore of a depth-heavy patch comes back whole
- **WHEN** a host calls setStateInformation with a saved state whose depths need more storage than the fresh plugin instance already has
- **THEN** every depth the saved state carries is live once the restore applies
