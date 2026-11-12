# Delta — `synth-browser-wasm-runtime`

<!-- check-lines-resolve -->

## MODIFIED Requirements

### Requirement: sbw-5 — MIDI: Web MIDI sysex multi-device bridge
WHEN browser MIDI support is enabled, THE browser runtime SHALL request MIDI access through `navigator.requestMIDIAccess({ sysex: true })`, enumerate multiple `MIDIInput` and `MIDIOutput` ports, map selected ports to the existing runtime MIDI instrument/controller configuration per controller slot, forward incoming MIDI bytes including sysex into the engine's existing MIDI input processor chain, send engine-produced MIDI output bytes including sysex through the selected `MIDIOutput` ports, and maintain desktop-equivalent polling/reconnect semantics without application-specific routing code.

THE browser runtime's per-action activation handler SHALL request MIDI access only when the dispatched action is the sidebar Controllers action, and SHALL NOT request it on audio activation or on any other dispatched action. At load, WHERE the Permissions API already reports Web MIDI sysex access as granted, THE browser runtime SHALL request MIDI access immediately, before any action dispatches; on any other reported state, on a rejected query, or where the Permissions API is unavailable, it SHALL request nothing until the Controllers action dispatches. A denied, unavailable, or not-yet-requested MIDI condition reached through this handler SHALL NOT prevent, delay, or fail audio activation, application startup, or the runtime instance.

#### Scenario: Sysex permission denial leaves MIDI offline
- **WHEN** Web MIDI sysex access is denied, unavailable, or blocked by permissions policy
- **THEN** the browser runtime keeps audio and UI running
- **AND** controller endpoints are reported as offline or unavailable through the generic runtime controller state
- **AND** status reports the generic offline MIDI state, with no permission-specific detail

#### Scenario: Incoming MIDI uses existing processors
- **WHEN** a selected browser MIDI input receives a message
- **THEN** the message bytes, including any sysex payload, are delivered to the matching controller slot's existing MIDI input processor chain
- **AND** resulting parameter/UI messages flow through the engine MIDI bus

#### Scenario: Outgoing MIDI stays generic
- **WHEN** an engine MIDI output processor emits bytes for a controller slot
- **THEN** the browser bridge sends those bytes to the selected `MIDIOutput` for that slot
- **AND** it does not inspect the concrete application type or widget layout

#### Scenario: Device changes reconcile endpoints
- **WHEN** Web MIDI reports a port connection or disconnection
- **THEN** the browser runtime updates the generic endpoint availability state and reconciles affected controller slots without rebuilding app-specific state
- **AND** it uses the existing JUCE-free MIDI reconciliation policy where the C++/WASM boundary permits

#### Scenario: Polling recovers missed device changes
- **WHEN** a browser MIDI input or output disappears or reappears without a reliable `statechange` delivery
- **THEN** the browser runtime's MIDI poll loop detects the changed port set
- **AND** configured controller slots move offline or reconnect using the same stored endpoint references

#### Scenario: Multiple devices stay independent
- **WHEN** two controller slots are mapped to different browser MIDI input/output port pairs
- **THEN** incoming messages and outgoing feedback for each slot use that slot's selected ports
- **AND** reconnecting one slot's port does not close or remap the other slot's active port

#### Scenario: MIDI access is requested only when Controllers opens
- **WHEN** the user dispatches an action that is not the sidebar Controllers action, including the action that starts audio
- **THEN** the browser runtime does not request MIDI access
- **AND** when the user subsequently dispatches the sidebar Controllers action, the browser runtime requests MIDI access at that point
- Check: `midi-flow.spec.ts: requests MIDI only when the Controllers sidebar action dispatches`

#### Scenario: Already-granted MIDI access starts silently at load
- **WHEN** the Permissions API reports Web MIDI sysex access as already granted
- **THEN** the browser runtime requests MIDI access at load, before the sidebar Controllers action is dispatched
- **AND** no permission prompt is shown
- Check: `midi-flow.spec.ts: starts MIDI at load only when the Permissions API already reports it granted`
