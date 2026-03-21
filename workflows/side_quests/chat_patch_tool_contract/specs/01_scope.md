# Scope

## Quest

- Name: `chat_patch_tool_contract`
- Main Quest: `obsidian_chat`
- Created: `2026-03-21`

## Summary

Clarify and possibly redesign the chat editing tool contract so agents stop sending incompatible patch formats and tool-event UX can rely on consistent file-operation behavior.

## Goals

- Document the observed mismatch between the server `apply_patch` tool and the patch format emitted by OpenAI-style coding agents.
- Decide whether to align the server tool contract to OpenAI-native patching, rename the current tool to an explicitly unified-diff contract, or add an adapter layer that accepts multiple patch dialects safely.
- Define the user-facing implications for chat transcript tool rendering so failed tool calls remain understandable and file-operation summaries stay consistent.

## Non-Goals

- Redesign the entire Obsidian chat pane.
- Change replica sync behavior.
- Solve general tool-use reliability across every non-filesystem tool in the same side quest.

## Open Questions

- Should the server adopt OpenAI-style patch application directly as the preferred editing contract for chat agents?
- If the server keeps unified diff as the underlying implementation, should the public tool be renamed to something explicit like `apply_unified_diff`?
- Would a compatibility layer that translates OpenAI-style patch envelopes into unified diffs be safe enough, or would it create too much ambiguity?
