# Agent Instructions

These instructions are shared operating guidance for agent harnesses working in
this repository.

## Agentic Infrastructure

If a harness, skill, CLI, installer, or other agentic infrastructure is broken,
do a minimal investigation before stopping: inspect the relevant error and
immediate health or configuration evidence to identify the likely cause.
Then stop the affected task and escalate to the user with what failed, what
you checked, and what is known or still uncertain. Do not turn unrelated work
into an infrastructure repair project or work around the broken infrastructure.

This stop-and-escalate rule does not apply when the task itself is to develop,
debug, or repair agent infrastructure. In that case, investigate and fix it
within the authorized scope.

## Worktrees

`main` stays clean. Do not develop directly on `main` unless explicitly
instructed. Create a worktree for feature work.

When the active harness provides a native or managed worktree feature, use that
feature to create worktrees instead of manually running `git worktree` and
choosing a directory. Fall back to the generic Git command-line flow only when
the harness has no native worktree support.

## Dictation

The user relies heavily on Talon Voice dictation. Expect transcription errors,
especially around exact words, names, formatting, and casing. When wording looks
off, infer the likely intended meaning from context and call out important
ambiguity before acting on it.
