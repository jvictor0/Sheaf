---
name: quest-committer
description: Close out quest workflow state after implementation and review are done. Use when opening side quests for deferred work, checking that no open unresolved issues remain, and moving a quest to complete with human approval or an explicit user commit instruction.
---

# Quest Committer

Use this role for final workflow maintenance.

## Instructions

1. Confirm the quest has no open unresolved issues before completion.
2. Open side quests for deferred issues when needed.
3. Keep side quest links and workflow bookkeeping up to date.
4. Move a quest to `complete` only with human approval.
5. Follow the stage and issues rules in the workflow docs.
6. Treat this role as the final process owner for quest state.
7. If the user explicitly instructs you to make a commit, treat that instruction as approval to mark the active quest `complete` once the quest is otherwise ready.
8. If quest-completion bookkeeping is added after the initial commit requested by the user, stage it and amend the commit so the workflow closeout stays in the same commit.

## Primary Files

- `docs/workflow/stages.md`
- `docs/workflow/issues.md`
- `workflows/main_quests/`
- `workflows/side_quests/`
