# Side Quest Directory Structure

This document defines the standard layout for a side quest.

## Purpose

A side quest is a smaller unit of follow-up work that comes from a main quest. It exists so a main feature can be considered done while deferred, adjacent, or cleanup work remains visible and actionable.

A side quest should answer:

- what follow-up work is being handled
- which main quest it belongs to
- which other side quests it relates to
- what decisions, issues, and specs define the work

Shared quest rules for `status.md`, `specs/`, `decisions.md`, `issues.md`, and naming are defined in `../quests.md`.

## Root Location

All side quests live under:

```text
workflows/side_quests/
```

A specific side quest lives under:

```text
workflows/side_quests/<side_quest_name>/
```

## Required Layout

```text
workflows/side_quests/<side_quest_name>/
├── decisions.md
├── issues.md
├── main_quest.md
├── related_side_quests.md
├── status.md
└── specs/
    ├── 01_scope.md
    ├── 02_design.md
    └── ...
```

## File Responsibilities

The shared quest files are documented in `../quests.md`.

### `main_quest.md`

This file links the side quest back to its owning main quest.

It should identify:

- the main quest name
- the path to the main quest directory
- the reason this side quest was split out
- the specific main quest issue or deferred item that caused it

### `related_side_quests.md`

This file links to other side quests that are adjacent, dependent, or overlapping.

Use it to capture:

- prerequisite side quests
- follow-on side quests
- parallel side quests touching the same area

If there are no related side quests yet, the file should still exist and say so explicitly.

## Relationship Rules

A side quest should always:

- link back to exactly one owning main quest in `main_quest.md`
- be listed from that main quest's `side_quests.md`
- maintain its own local issues and decisions

A side quest may:

- link to multiple related side quests in `related_side_quests.md`
- remain open after the owning main quest is complete

## Example

```text
workflows/side_quests/file_logging_repair_scan/
├── decisions.md
├── issues.md
├── main_quest.md
├── related_side_quests.md
├── status.md
└── specs/
    ├── 01_scope.md
    └── 02_validation.md
```
