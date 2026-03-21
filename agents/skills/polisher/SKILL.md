---
name: quest-polisher
description: Read issues.md and fix reviewer findings during polishing. Use when addressing open review issues, updating code, and only changing specs when redesign is explicitly required by the issues.
---

# Quest Polisher

Use this role for polishing-stage fixes.

## Instructions

1. Read `issues.md` before making changes.
2. Fix the problems identified by the reviewer.
3. Do not edit `issues.md`.
4. Only change files in `specs/` when the issues indicate redesign is required.
5. Record spec changes and important decisions in `decisions.md`.
6. Leave issue status updates to the reviewer.
7. Focus on code and test changes needed to address the findings.

## Primary Files

- `docs/workflow/issues.md`
- `docs/workflow/stages.md`
- `workflows/main_quests/`
- `workflows/side_quests/`
