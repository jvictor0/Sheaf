# Delta — `synth-parameter-modulation`

The low watermark for depth storage is a fixed multiple of the modulator
count, sized for one encoder press; an app whose one press adds many depths
needs a larger number, not a new mechanism. The command model gains the
app command. The modified requirement restates its promoted text whole. A
storage batch supplied on the message thread was appended to a vector the
audio thread may be walking, and a thread-sanitizer run confirmed the race
through the drill-in path; the group also marked a storage request pending
only after pushing it, so a batch supplied between the push and the mark
left the group marked pending for good and unable to ask again once free
slots ran out.

## MODIFIED Requirements

### Requirement: spm-22 — Message input: command model
WHEN external UI or MIDI code sends commands to the synth parameter system, THE system SHALL represent each command as a timestamped `MessageIn` with no route field and with typed support for `ParamIncDec`, `ParamPush`, `ToggleReset`, `SetReset`, `ToggleRandom`, `SetRandom`, `ToggleRandomMod`, `SetRandomMod`, `ToggleGestureSelect`, `SetGestureSelect`, `SelectParamBank`, `Start`, `Stop`, `Clock`, `SetGestureValue`, `SceneSelect`, `SetSceneBlend`, `AppAction` (dispatched to the app's surface on the message thread), and `AppCommand` (an app-defined command number and a float value, applied by the app on the audio thread and never interpreted by the system), with every new type appended after the last so existing ordinals are unchanged.

#### Scenario: Parameter messages carry slot and position
- **WHEN** a parameter inc/dec or push message is created
- **THEN** the message carries the target slot index and visible position
- **AND** does not require a physical encoder ID from the sender

#### Scenario: Slot position maps through slot encoder order
- **WHEN** a parameter message targets slot position `i`
- **THEN** the manager resolves position `i` to the physical encoder at index `i` in that slot's `AddPhysicalEncoder` order
- **AND** routes the resolved physical encoder ID through the selected bank's visible cells

#### Scenario: Reset messages can toggle or set explicit state
- **WHEN** a reset toggle message is created
- **THEN** the message carries no required boolean payload
- **WHEN** an explicit reset set message is created
- **THEN** the message carries a boolean payload indicating the desired reset-held state

#### Scenario: Random messages can toggle or set explicit state
- **WHEN** a random toggle message is created
- **THEN** the message carries no required boolean payload
- **WHEN** an explicit random set message is created
- **THEN** the message carries a boolean payload indicating the desired random-held state

#### Scenario: Random-mod messages can toggle or set explicit state
- **WHEN** a random-mod toggle message is created
- **THEN** the message carries no required boolean payload
- **WHEN** an explicit random-mod set message is created
- **THEN** the message carries a boolean payload indicating the desired random-mod-held state

#### Scenario: Gesture messages carry gesture index and optional explicit selection state
- **WHEN** a gesture select toggle or gesture value message is created
- **THEN** the message carries the gesture index
- **AND** the value-setting message also carries the normalized gesture value
- **WHEN** an explicit gesture select message is created
- **THEN** the message carries the gesture index
- **AND** carries a boolean payload indicating the desired selected state

#### Scenario: Bank selection carries slot and bank
- **WHEN** a parameter bank selection message is created
- **THEN** the message carries which slot to set and which bank index to select
- **AND** the bank index refers to the manager's global bank list

#### Scenario: Scene selection carries one ordinal
- **WHEN** a scene selection message is created
- **THEN** the message carries one scene ordinal
- **AND** does not change scene blend unless a scene blend message is also processed

#### Scenario: An app command carries the app's number and a value
- **WHEN** `MessageIn::AppCommand(timestamp, command, value)` is created
- **THEN** the message carries that number and value and no string
- **AND** no controller profile can address it

## ADDED Requirements

### Requirement: spm-95 — Storage: the low watermark is a group setting
THE parameter group SHALL let an app set the available-slot count below which an allocation requests a storage batch (`SetStorageLowWatermark`), SHALL default it to twice the modulator count, and SHALL use it in the existing low-water request at every local allocation.

#### Scenario: A raised watermark requests below it
- **WHEN** the watermark is set to 100 and an allocation leaves 89 available slots
- **THEN** a batch request for the shortfall is pushed; with the watermark back at its default and available between the default and 100, none is

### Requirement: spm-91 — Storage: a batch supplied while the audio thread reads
WHEN a parameter storage batch is supplied to a group on the message thread while the audio thread allocates, finds or counts that group's parameters, THE synth parameter modulation system SHALL append the batch without moving, reallocating or invalidating any storage the audio thread can reach, SHALL publish the new batch to the audio thread only once it is fully built, and SHALL track the pending-request state with an atomic, so that no data race exists between the two threads; THE group SHALL mark a request pending before it pushes the request, and clear the mark when the push fails, so that a batch supplied at once always leaves the group able to ask again.

#### Scenario: Existing storage stays put
- **WHEN** a batch is supplied after parameters have been allocated from earlier batches
- **THEN** every earlier parameter keeps its address and local index, and the group's available slot count grows by the new batch's capacity
- Check: `tests/parameter_modulation_tests.cpp: a_later_storage_batch_does_not_move_parameters_allocated_from_an_earlier_one`

#### Scenario: A batch supplied at once leaves the group able to ask again
- **WHEN** a group requests storage and the batch is added before the requesting call returns
- **THEN** the group is not marked pending afterwards, and its next shortfall pushes a new request
- Check: none automated: the interleaving cannot be driven from one thread, and a thread-sanitizer run does not see a lost update on an atomic; read `ParameterGroup::RequestParameterStorageBatch`'s store-before-call ordering directly.
