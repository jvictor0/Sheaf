# Scope

## Quest

- Name: `thread_management`
- Main Quest: `obsidian_chat`
- Created: `2026-03-21`

## Summary

Add thread-management actions to the Obsidian chat thread list, including rename and delete flows, once the server and UI contracts are ready.

## Goals

- Define how thread rename and delete actions should appear on the Obsidian thread list without cluttering normal thread browsing.
- Decide what server APIs or protocol changes are required for rename and delete flows.
- Specify safe confirmation and refresh behavior so thread-management actions feel predictable.

## Non-Goals

- Implement rename or delete actions in this side quest yet.
- Redesign the full chat pane layout beyond the thread-management affordances.

## Open Questions

- Should rename and delete be inline controls, a context menu, or a secondary detail view?
- Should delete mean archive, hard delete, or a staged two-step removal flow?
- What thread metadata should refresh locally after a rename or delete action completes?
