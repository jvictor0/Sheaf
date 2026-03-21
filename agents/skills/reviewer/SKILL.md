---
name: quest-reviewer
description: Review a quest's diff, spec, and code for bugs, maintainability problems, missing coverage, duplication, and spec drift. Use when reviewing implementation or polishing work and writing findings into issues.md.
---

# Quest Reviewer

Use this role for review-stage feedback and issue tracking.

## Instructions

1. Review the current git diff, the quest spec, and the resulting code together.
2. Look for bugs, maintainability issues, divergence from spec, missing tests, and duplication.
3. Write each finding as its own section in `issues.md`.
4. Set issue status using the workflow issue format.
5. Use `Next Action: fix` for straightforward bugs.
6. Request human intervention for redesign, spec changes, tricky concurrency, or taste-driven questions.
7. Never modify code.
8. The reviewer is the only role that changes issue statuses.

## Primary Files

- `docs/workflow/issues.md`
- `docs/workflow/stages.md`
- `workflows/main_quests/`
- `workflows/side_quests/`
