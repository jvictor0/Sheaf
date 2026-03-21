# Scope

## Quest

- Name: `legacy_chat_rest_contract`
- Main Quest: `obsidian_chat`
- Created: `2026-03-21`

## Summary

Resolve the contract drift between the live websocket-first chat server and the
older iOS model helpers that still reference thread metadata and message REST
endpoints the server no longer exposes.

## Goals

- Confirm whether any active clients still depend on `GET /threads/{id}/metadata`
  and `GET /threads/{id}/messages`.
- Decide whether the long-term contract should stay websocket replay plus
  committed turns only, or whether the server should restore REST history
  helpers for compatibility.
- Record the migration path so future chat clients do not need to reverse
  engineer which history surface is authoritative.

## Non-Goals

- Rework the Obsidian pane rendering.
- Redesign the websocket chat protocol already documented for the main quest.
- Implement broader iOS client UX changes beyond the contract decision.

## Open Questions

- Are the legacy REST helpers still exercised anywhere outside stale iOS model
  code?
- If the endpoints remain retired, what cleanup or deprecation work should land
  in the iOS client?
- If the endpoints return, should they expose the same committed-turn shape as
  websocket replay or a reduced compatibility payload?
