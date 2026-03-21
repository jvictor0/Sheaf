---
name: quest-polisher
description: Read issues.md and fix reviewer findings during polishing. Use when addressing open review issues, updating code, and only changing specs when redesign is explicitly required by the issues.
---

# Quest Polisher

Use this role for polishing-stage fixes.

## Instructions

1. Read `issues.md` before making changes.
2. Only act on issues whose status is `open`.
3. Do not act on issues marked `deferred`, `rejected`, or `completed`.
4. Fix the open problems identified by the reviewer.
5. Do not edit `issues.md`.
6. Only change files in `specs/` when the open issues indicate redesign is required.
7. Record spec changes and important decisions in `decisions.md`.
8. Leave issue status updates to the reviewer.
9. Focus on code and test changes needed to address the open findings.
10. Run appropriate tests for the fixes you make (for example the project’s unit or integration tests, or any test commands documented for the quest).

## Primary Files

- `docs/workflow/issues.md`
- `docs/workflow/stages.md`
- `workflows/main_quests/`
- `workflows/side_quests/`
