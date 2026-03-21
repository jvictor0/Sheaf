# Workflow System

This directory documents how work should be organized in `workflows/`.

The goal is to keep feature work structured, traceable, and easy to inspect at a glance. A feature starts as a `main_quest`. A completed `main_quest` can still have unresolved follow-up work, and that follow-up is tracked as one or more `side_quests`.

## Overview

- Workflow data lives under `workflows/`
- Top-level workflow groups are `workflows/main_quests/` and `workflows/side_quests/`
- Each `main_quest` represents a feature-level effort
- Each `side_quest` represents scoped follow-up work derived from a `main_quest`
- Both quest types keep their own specs, decisions, issues, and status
- Stage behavior and change controls are defined in `stages.md`
- Issue tracking format is defined in `issues.md`
- Shared quest file rules are defined in `quests.md`

## Main Quest Structure

Each main quest lives at:

```text
workflows/main_quests/<feature_name>/
```

Each main quest directory contains:

- `specs/` for one or more markdown specification documents
- `decisions.md` for recorded design or scope decisions
- `issues.md` for outstanding issues, risks, and unresolved questions
- `status.md` for current stage tracking
- `side_quests.md` for side quests spun out from unresolved or deferred work

The allowed stages in `status.md` are:

- `planning`
- `implementation`
- `polishing`
- `complete`

## Main Quest Completion

A main quest can be marked `complete` even if some follow-up work remains.

In that case:

- unresolved items remain documented in `issues.md`
- the actionable follow-up work is broken out into one or more side quests
- `side_quests.md` lists the related side quests created for that remaining work

This makes it possible to close the main feature diff while still preserving visibility into unfinished adjacent work.

## Side Quest Structure

Each side quest lives at:

```text
workflows/side_quests/<side_quest_name>/
```

Each side quest directory mirrors the main quest structure, but also adds explicit linkage files:

- `specs/`
- `decisions.md`
- `issues.md`
- `status.md`
- `main_quest.md` linking back to the owning main quest
- `related_side_quests.md` linking to adjacent or dependent side quests

## Recommended Naming

- Use stable, readable directory names such as `file_logging`, `obsidian_replica`, or `repair_missing_metadata`
- Prefer underscores over spaces in workflow directory names
- Keep side quest names specific to the follow-up being handled

## Scaffolding

Use `scripts/create_workflow.py` to create new workflow directories with the standard markdown files.

Create a main quest:

```bash
python3 scripts/create_workflow.py main file_logging_cleanup --summary "Track follow-up cleanup work for file logging."
```

Create a side quest:

```bash
python3 scripts/create_workflow.py side repair_missing_metadata --main-quest file_logging --summary "Handle deferred metadata repair work."
```

Notes:

- names are normalized to lowercase underscore form
- new quests start in `planning`
- side quest creation requires an existing owning main quest
- after creating a side quest, add it to the owning main quest's `side_quests.md`

## Documents In This Directory

- `issues.md`
- `quests.md`
- `stages.md`
- `main_quests/directory_structure.md`
- `side_quests/directory_structure.md`

These documents define the expected contents and responsibilities of each quest type in more detail.
