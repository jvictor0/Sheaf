# Workflow Stages

This document defines the meaning of each workflow stage and the change controls that apply while a quest is in that stage.

These rules apply to both `main_quests` and `side_quests`.

## Stage List

- `planning`
- `implementation`
- `polishing`
- `complete`

## Planning

The `planning` stage is for defining the work before implementation starts.

During `planning`:

- markdown documents may be written and updated
- files in `specs/` may be written and updated
- non-markdown implementation files must not be created or modified

The intent of this stage is to allow design, scoping, and issue shaping without beginning code changes.

## Implementation

The `implementation` stage is for executing against an approved spec.

During `implementation`:

- implementation files may be created and modified
- workflow markdown files such as `decisions.md`, `issues.md`, `status.md`, and related tracking files may be updated
- files in `specs/` are read-only and must not be modified

The intent of this stage is to treat the spec as the fixed contract for the work being implemented.

## Polishing

The `polishing` stage is for cleanup, validation, and constrained follow-up adjustments after implementation is substantially done.

During `polishing`:

- implementation files may still be improved
- workflow tracking files may still be updated
- any change to a file in `specs/` requires human review and approval before the change is accepted
- every approved spec change made during `polishing` must be documented in `decisions.md`

The intent of this stage is to avoid casual spec drift while still allowing controlled corrections when a human explicitly approves them.

## Complete

The `complete` stage means the quest is closed as an active workflow item.

Moving a quest to `complete` requires human approval.

A quest may be marked `complete` even if follow-up work still exists, as long as:

- the main goal of the quest is done
- remaining issues are documented
- any deferred actionable work has been captured as side quests when appropriate

## Approval Expectations

Human approval is required for:

- any spec change made while a quest is in `polishing`
- any transition of a quest into `complete`

When approval affects a design or lifecycle decision, record that approval in `decisions.md`.
