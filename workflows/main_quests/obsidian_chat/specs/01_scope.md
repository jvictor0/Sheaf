# Obsidian Chat Problem And Scope

## Quest

- Name: `obsidian_chat`
- Created: `2026-03-21`

## Summary

Add a chat pane to the existing Obsidian plugin so a user can browse threads,
enter a thread, send messages to the agent, watch assistant tokens stream in
incrementally, inspect past chat history, switch threads, and create a new
thread without leaving Obsidian.

This feature should match the iOS chat experience and chat metadata structures
as closely as practical while using the current Python server's websocket-based
chat transport as the source of truth.

## Problem

The repository already contains:

- a working Obsidian plugin focused on replica sync and vault protections
- a working iOS client for chat
- a working server-side chat transport with REST thread entry and websocket
  streaming

What is missing is an Obsidian-native chat surface that uses the same thread
and websocket flow as the iOS app instead of creating a second, plugin-specific
chat protocol or UX model.

## Goals

- Add a dedicated Obsidian chat pane inside the existing plugin.
- Show a thread list when the pane opens.
- Let the user select an existing thread and enter that conversation.
- Let the user return from the conversation view to the thread list.
- Let the user create a new thread and immediately enter it.
- Show committed chat history for the selected thread.
- Send messages from a text input using Enter.
- Stream assistant output incrementally while generation is in progress.
- Match the iOS client state model closely enough to preserve reconnect,
  handshake replay, pending-send, and streaming semantics.
- Document the websocket chat contract in detail for future implementation and
  maintenance work.
- Keep the initial rendering model intentionally simple and reliable.

## Required User Experience

### Pane Entry

When the user opens the chat pane:

- the default screen is the thread list
- the thread list loads from the server
- a gear icon is available to open configuration for chat-related settings such
  as model selection
- the user can select a thread to enter it
- the user can start a new thread from this screen

### Conversation Screen

Inside a selected thread:

- previous committed messages are shown in chat order
- user messages render as bubbles
- assistant messages render in a ChatGPT-like assistant style
- tool calls and other tool-related events render as bubbles
- the composer contains a text input area
- pressing Enter sends the message
- the user can navigate back to the thread list
- the user can switch to another thread by returning to the list

### In-Progress Generation

While a response is being generated:

- a lightweight thinking indicator is visible while thinking tokens are being received
- streamed tokens appear incrementally in an in-progress assistant container
- an activity animation indicates that the system is still working
- once the assistant turn is finalized, the temporary streamed container may be
  replaced by a neatly rendered committed message

### Tool Call Rendering

When assistant turns include tool-call metadata:

- render lightweight tool-call bubbles or event rows near the assistant output
- for file-oriented tools such as read, write, and patch, show only the
  filename, not file contents
- do not display raw tool arguments in cases where that would expose more than
  the filename requirement allows

## Protocol Parity Goal

The feature should reuse the existing chat flow already used by the iOS app:

- `GET /threads`
- `POST /threads`
- `POST /threads/{thread_id}/enter-chat`
- `WS /ws/chat/{session_id}`

The Obsidian chat pane must not invent a second transport for the same feature.

It is also important that chat transport and replica file-sync transport remain
parallel but separate streams:

- chat uses its own websocket session for thread replay and live assistant
  streaming
- replica sync uses its own websocket session for file log replay and vault
  synchronization
- the plugin may have both connections active at the same time

## State Parity Goal

The Obsidian pane should maintain the same core in-memory chat concepts used by
the iOS app, including:

- committed turns
- pending sends
- streaming assistant text keyed by queue ID
- last committed turn ID
- thread summary metadata
- per-thread cached rendered session state where useful

The cache remains memory-only in v1 and should be treated as a convenience for
live UI state, not as durable chat storage. After plugin reload or a fresh pane
entry, the transcript and thread list should be refreshed from the server.

## Non-Goals

- Rich markdown parity with the iOS renderer
- LaTeX rendering
- Full reasoning-trace display in the UI
- Exposing raw tool results or full tool payloads in the transcript
- Merging the chat transport with the replica sync websocket
- Redesigning the server chat protocol as part of the first Obsidian pass
- Broad chat search, archival management UI, or thread deletion UI
- Offline-first queued chat composition

## Design Constraints

- The feature lives in the existing `apps/obsidian-replica` plugin.
- The plugin remains mobile-conscious and should avoid design choices that only
  work on desktop.
- The implementation should prefer server truth and committed turns over local
  optimistic transcript state.
- Simplicity and behavioral parity matter more than advanced visual polish in
  the first pass.

## Deliverables

This main quest should produce planning docs for:

- user-facing pane behavior
- client state and rendering model
- protocol and payload contract
- Obsidian plugin architecture and integration points
- rollout and validation guidance

## Resolved Planning Decisions

- v1 uses a gear icon that takes the user to configuration for model settings
  rather than adding an inline model selector to the chat pane
- the thread list refreshes automatically every time the list view is shown
- chat caching remains memory-only and should not persist transcript UI state
  across plugin reloads
