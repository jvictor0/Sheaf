# Delta — `synth-app-runtime`

An app's own command has no audio-thread delivery: `AppAction` is forwarded
back to the message thread by design. Apps build a bridge of their own and
lose count and order inside one message tick. The requirements below give
an app command the bus route tempo already has and expose the clock state
the engine already publishes.

## ADDED Requirements

### Requirement: sar-38 — App commands: audio-thread delivery in bus order
WHEN an app declares `HasAppCommands` (`ApplyAppCommand(std::size_t command, float value)`), THE engine SHALL hand every `MessageIn::AppCommand` popped from either message bus to that hook on the audio thread, in the block that pops it, in that bus's FIFO order with every other non-realtime message on it, before the app's `ProcessFrame` and `ProcessBlock` for that block. Start, Continue, Stop and Clock SHALL remain realtime messages, lifted out of both buses and applied after both have drained, as today. The command number SHALL be the app's own and the engine SHALL NOT interpret it. No message SHALL be held at the head of a bus. An app without the hook SHALL see the command dropped by the bus, since only that app's surface produces one.

#### Scenario: A command and a turn keep their order
- **WHEN** an app command and a `ParamIncDec` are pushed to the UI bus in that order
- **THEN** both are applied in the next block, the command before the turn

#### Scenario: A transport message pushed after a command still lands in the same block
- **WHEN** a command and a `Start` are pushed to the UI bus in that order
- **THEN** the command is applied in the drain and the transport is running after that block's realtime batch

### Requirement: sar-40 — Context: clock and sync state readable by the app's UI
THE engine SHALL expose through `AppContext` the clock diagnostics publication it already writes once per block (`currentBpm`, and `transportState`, filled by the master clock) and the requested sync configuration, each field stating its thread as the other context fields do, so an app's message-thread UI reads tempo, external-clock and transport-running state from the engine and keeps no mirror of the master clock in atomics of its own.

#### Scenario: Transport state reaches the UI
- **WHEN** `Start` is applied on the rig
- **THEN** the context's clock diagnostics snapshot reads `Running` after that block, and `Stopped` after a `Stop`
