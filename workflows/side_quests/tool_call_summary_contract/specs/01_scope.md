# Scope

## Quest

- Name: `tool_call_summary_contract`
- Main Quest: `obsidian_chat`
- Created: `2026-03-21`

## Summary

Define a stable user-facing summary contract for file-oriented tool calls so the
chat transcript can show helpful file labels without leaking file contents, raw
payloads, or unstable path details.

## Goals

- Specify which tool families count as file-oriented for transcript rendering.
- Define the preferred path extraction order for tool payloads such as
  `relative_path`, `relative_dir`, `path`, and other tool-specific fields.
- Decide when the UI should show a vault-relative path versus only a basename,
  and document safe fallbacks when no trustworthy path can be derived.
- Capture validation examples so implementations can share one summary helper
  and tests can assert the same output across clients.

## Non-Goals

- Redesign the full chat bubble layout.
- Expose raw tool payloads or file contents in the UI.
- Solve the separate patch-dialect mismatch already tracked in
  `chat_patch_tool_contract`.

## Open Questions

- Should vault-local files display as vault-relative paths or basename-only
  labels in v1?
- Which tool argument keys are authoritative when multiple path-like values are
  present?
- Should directory-oriented tools such as `list_notes` follow the same privacy
  rules as single-file tools, or a narrower directory-summary variant?
