# Obsidian Plugin Architecture Plan

## Scope

This document outlines how the new chat feature should fit into the existing
`apps/obsidian-replica` plugin without disturbing the current replica-sync
design.

## Architectural Position

The chat feature is an additive module inside the existing plugin.

It should:

- share plugin lifecycle and settings infrastructure
- use a separate service layer from replica sync
- use the existing server chat transport, not the replica transport
- remain safe for both desktop and mobile-compatible plugin targets

The plugin architecture should explicitly assume two parallel websocket-backed
flows may be active:

- one chat websocket for thread replay and live assistant generation
- one replica websocket for file synchronization

These flows must remain operationally independent even when both are connected
at the same time.

## Proposed Plugin Surface

### New View

Add a dedicated Obsidian pane/view for chat.

Responsibilities:

- host thread list UI
- host active conversation UI
- wire user actions to chat state/services
- refresh the DOM when state changes

Likely Obsidian primitives:

- `ItemView`
- `registerView(...)`
- command to open the chat pane
- optional ribbon or workspace entry point if desired later

### Existing Settings Tab

Reuse the plugin settings tab for chat-related configuration where needed, such
as:

- server base URL reuse
- model selection reached from the chat pane through a gear icon that navigates
  to configuration
- optional chat behavior toggles introduced during implementation

The pane itself should not depend on desktop-only settings access to function.

## Proposed Module Split

Implementation file names may differ, but planning should assume these
responsibilities exist.

### Chat API Layer

Responsibilities:

- `GET /threads`
- `POST /threads`
- `POST /threads/{thread_id}/enter-chat`
- server response decoding

### Chat Transport Layer

Responsibilities:

- websocket connect/disconnect
- frame decode
- callback or subscription model for chat events
- heartbeat tracking
- message submission
- reconnect support using `known_tail_turn_id`

### Chat State Store

Responsibilities:

- own thread list state
- own active-thread state
- mirror iOS concepts such as committed turns, pending sends, streaming buffers,
  and last committed tail
- expose deterministic view state for rendering

### View Renderer

Responsibilities:

- render thread list
- render transcript
- render pending sends
- render streaming assistant content
- render tool-event summaries
- render thinking and activity indicators
- update visible regions incrementally instead of rebuilding the entire chat pane on every transport frame

## State Ownership

### Plugin-Level State

The plugin instance should own:

- chat services
- the registered chat view
- shared configuration
- optionally an in-memory per-thread session cache

### Per-Thread Session State

Each active thread session should track:

- thread metadata
- committed turns
- pending sends
- streaming state keyed by queue ID
- last committed turn ID
- connection and error state

### Session Cache Strategy

To match iOS behavior closely, keep a lightweight in-memory session cache keyed
by thread ID.

For v1:

- in-memory caching is sufficient
- durable transcript persistence is not required
- clearing stale sessions on plugin unload is acceptable
- on pane reopen or thread re-entry, refresh authoritative state from the server

## DOM And Rendering Strategy

The user explicitly does not need advanced markdown or math rendering in v1.

Recommended rendering approach:

- create the pane shell once per view instance and keep header, transcript,
  status, and composer in stable DOM containers
- render user and assistant messages with separate CSS classes
- keep committed transcript rows keyed by turn ID and leave them mounted once
  committed during normal live updates
- render streaming assistant content in a mutable container that can be updated
  in place
- preserve the composer DOM node, draft text, and focus state across normal
  chat updates
- reserve full transcript rebuilds for explicit replay/reset moments such as
  handshake snapshot begin, reconnect resync, or thread switch
- when a committed final assistant turn arrives, replace or redraw only the
  affected in-progress row from the committed turn content

This keeps the rendering loop simple and stable for the first implementation,
while also avoiding focus loss and scroll jumps during routine websocket
traffic.

## Incremental Update Rules

Normal event handling should prefer narrow DOM updates:

- `heartbeat` and other non-visible transport frames should not trigger visible
  rerenders
- `assistant_token` should update only the active streaming row and any
  user-visible activity indicator
- `committed_turn` should append or reconcile only the affected turn rows and
  matching pending-send artifacts
- status text changes should update the status region only
- the composer should not be recreated during live transcript updates

## Scroll And Focus Rules

Scrolling should be conditional and user-respecting:

- do not force scroll-to-bottom on every render cycle
- only auto-scroll when new visible transcript content appears and the user was
  already near the bottom before the update
- if the user has scrolled upward, preserve their scroll position during
  streaming and status updates
- preserving composer focus takes priority over transcript redraw convenience

## Thread Navigation Strategy

### Pane Open

When the pane opens:

- show thread list first
- fetch latest threads
- show a gear/settings affordance for configuration entry
- do not auto-enter the last thread unless a future explicit UX choice is added

### Enter Thread

When the user selects a thread:

- close any existing thread websocket if another thread is active
- load or initialize the thread session state
- call `enter-chat`
- open websocket
- replay committed turns via handshake

### Return To List

When the user returns to the list:

- leave the transcript screen cleanly
- refresh the thread list from the server automatically
- either disconnect immediately or keep the transport alive only if the final
  implementation proves it is valuable and safe
- do not leave hidden duplicate transports running

Planning default:

- disconnect on leave for simplicity
- reconnect on re-entry using `lastCommittedTurnID`

## Send Flow

When the user submits a message:

1. trim input text
2. reject empty sends
3. create a local pending-send record
4. render a local user bubble immediately
5. submit websocket `submit_message`
6. wait for `message_durable_ack`
7. append streaming assistant chunks as they arrive
8. replace local pending/streaming artifacts when authoritative committed turns
   arrive

## Conflict And Error Recovery

### Execution Conflict

On `execution_conflict`:

- clear uncommitted pending and streaming artifacts for the active thread
- reconnect using the last known committed tail
- rebuild the transcript from replay

### Silent Connection Staleness

The iOS client uses a simple last-frame watchdog. The Obsidian implementation
should plan for the same behavior:

- track time of last received frame
- if the websocket goes quiet beyond the allowed interval, reconnect
- use `lastCommittedTurnID` to resume

### Fatal Error

On fatal queue/send failure:

- stop showing thinking/streaming indicators
- keep already committed transcript entries intact
- present a concise error state in the pane

## Tool Event Summary Helper

The Obsidian implementation should define a dedicated summarizer for tool calls.

Responsibilities:

- classify file-oriented tool names
- extract basename only
- produce short user-safe labels
- avoid accidentally leaking raw file contents through tool `result` rendering

This helper should be shared anywhere tool calls are rendered so behavior is
consistent across committed history and live updates.

## Styling Expectations

The styling target is intentionally modest:

- user bubbles
- assistant bubble area with readable width and spacing
- subtle tool-event row styling
- small thinking indicator
- small streaming activity animation

The first pass should favor clarity and robustness over animation-heavy polish.

## Interaction With Replica Sync

The plugin already has a replica sync client and websocket flow.

Rules for this quest:

- do not reuse replica message types for chat
- do not combine chat and replica traffic on one websocket
- keep chat service ownership separate from replica synchronization ownership
- keep configuration reuse explicit instead of implicit transport coupling

## Testable Architecture Outcomes

The architecture should make these units independently testable:

- REST request/response decoding
- websocket frame decoding
- state reducer or event application logic
- tool-call summary formatting
- thread switch and reconnect behavior
- incremental transcript updates without full-pane rerender on heartbeat or
  status-only events
