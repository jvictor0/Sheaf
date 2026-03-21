# Status

- Stage: `planning`
- Updated: `2026-03-21`
- Summary: Define the full main-quest plan for adding an iOS-parity chat pane to the existing Obsidian plugin, including thread list UX, websocket streaming chat behavior, client state, and protocol documentation.

## Planning Exit Criteria

- The quest has a detailed scope and non-goals document for `Obsidian Chat`.
- The expected Obsidian pane UX is specified for thread list, thread entry, sending, switching, and new-thread creation.
- The websocket chat contract is documented in enough detail that an implementer does not need to reverse-engineer the Python server or iOS client during implementation.
- The planned in-memory state matches the iOS chat model closely enough to preserve reconnect and streaming behavior.
- Validation and rollout steps are written down for desktop and mobile-safe Obsidian behavior.
