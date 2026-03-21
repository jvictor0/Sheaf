# Issues Document

This document defines the structure of the workflow issues document.

The issues document is the quest `issues.md` file.

## Purpose

The issues document tracks issue-level feedback, unresolved work, and human guidance for a quest.

Each issue is represented as its own markdown section.

## Issue Structure

Each issue should be written as a section in `issues.md`.

Example:

```md
## Missing retry backoff cap

Describe the issue here.

Status: `open`

Next Action: `fix`
```

## Allowed Status Values

Each issue section must include one of these statuses:

- `open`
- `completed`
- `deferred`
- `rejected`

## Next Action

Each issue section must end with a `Next Action` field.

This field is for human comments and direction about what should happen next.

Guidelines:

- start new issues with a default value such as `fix`
- a human may later replace that default with a more specific instruction
- keep the field at the bottom of the issue section so it is easy to scan and update

## Recommended Pattern

- use one section per issue
- use a concise section title
- include enough context to understand the problem without reopening other docs
- update `Status` as the issue moves forward
- update `Next Action` when a human clarifies the expected next step
