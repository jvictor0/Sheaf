# Quests

This document defines the workflow rules shared by both `main_quests` and `side_quests`.

For quest-specific structure, also see:

- `main_quests/directory_structure.md`
- `side_quests/directory_structure.md`

## Shared Files

All quests include these shared files:

- `status.md`
- `specs/`
- `decisions.md`
- `issues.md`

## `status.md`

This file records the current lifecycle stage of the quest.

Allowed values:

- `planning`
- `implementation`
- `polishing`
- `complete`

Recommended contents:

- current stage
- short summary of the current objective
- last meaningful update
- completion criteria or exit conditions for the current stage

Stage behavior and approval rules are defined in `stages.md`.

## `specs/`

This directory contains the specification documents for the quest.

Guidelines:

- use as many markdown files as needed
- split documents by concern when the quest is large
- keep filenames ordered when reading order matters
- treat this directory as the canonical design and behavior definition for the quest

Good examples:

- `01_problem_and_scope.md`
- `02_data_model.md`
- `03_api_and_rpc.md`
- `04_rollout_and_validation.md`

## `decisions.md`

This file records decisions made while shaping or implementing the quest.

Each decision entry should ideally capture:

- the decision
- why it was made
- alternatives considered
- any consequences or follow-up work created by the decision

## `issues.md`

This file is the issues document for the quest.

It tracks unresolved work, risks, and human guidance related to the quest.

An issue may remain here even after the quest is marked complete.

Issue format rules are defined in `issues.md`.

## Naming Guidance

- directory names should be stable and descriptive
- prefer lowercase with underscores
- use specific quest names rather than generic labels
