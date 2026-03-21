# Main Quest Directory Structure

This document defines the standard layout for a main quest.

## Purpose

A main quest is the top-level workflow unit for a feature. It should answer:

- what feature is being built
- what the current state is
- what decisions have been made
- what issues remain open
- what side quests were created from deferred or leftover work

Shared quest rules for `status.md`, `specs/`, `decisions.md`, `issues.md`, and naming are defined in `../quests.md`.

## Root Location

All main quests live under:

```text
workflows/main_quests/
```

A specific feature lives under:

```text
workflows/main_quests/<feature_name>/
```

## Required Layout

```text
workflows/main_quests/<feature_name>/
├── decisions.md
├── issues.md
├── side_quests.md
├── status.md
└── specs/
    ├── 01_overview.md
    ├── 02_execution_model.md
    └── ...
```

## File Responsibilities

The shared quest files are documented in `../quests.md`.

### `side_quests.md`

This file is the bridge between the main quest and any follow-up work split out after or during implementation.

It should list:

- side quest name
- short reason it exists
- current status
- link to the side quest directory

If the main quest has no side quests yet, the file should still exist and state that none have been created.

## Completion Rule

A main quest may move to `complete` without every issue being fully resolved.

That is allowed only when:

- the main feature goal is done
- the remaining work is clearly bounded
- the remaining work is preserved in `issues.md`
- the actionable follow-up has been converted into one or more side quests and listed in `side_quests.md`

## Example

```text
workflows/main_quests/file_logging/
├── decisions.md
├── issues.md
├── side_quests.md
├── status.md
└── specs/
    ├── 01_write_log_core.md
    ├── 02_replay_strategy.md
    └── 03_repair_rules.md
```
