# Roles

These are the repository roles for workflow-driven quest execution.

Runs are currently human-orchestrated and happen one at a time.

## Planner

- interactive role
- only writes markdown files and spec files
- creates new quests
- updates spec files based on user input
- owns planning-stage work

## Implementer

- reads the spec
- creates an implementation plan and executes it
- must implement the full spec before finishing
- never changes the spec
- does not handle issue responses
- records implementation decisions in `decisions.md`

## Reviewer

- runs against the same git state as the implementer or polisher
- reviews the diff, spec, and code
- looks for bugs, maintainability issues, divergence from spec, missing tests, and duplication
- writes findings to `issues.md`
- assigns `Next Action`
- uses `fix` for straightforward bugs
- requests human intervention for redesign, tricky concurrency, spec changes, or taste-driven questions
- never changes code
- is the only role that changes issue statuses

## Polisher

- reads `issues.md`
- fixes the problems found by the reviewer
- does not change `issues.md`
- only changes specs when the issues indicate redesign is required
- updates `decisions.md` when changing the spec or making important decisions

## Committer

- maintains the overall process
- opens side quests for deferred issues
- ensures there are no open unresolved issues before completion
- moves quests to their final completed stage

## Reviewer And Polisher Loop

- the reviewer and polisher may iterate as many times as needed
- the human orchestrates the sequence
- there is no automated back-and-forth loop yet
