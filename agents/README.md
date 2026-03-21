# Agent Roles And Skills

This directory is the canonical home for repository-specific agent roles and skills.

## Layout

- `roles/` contains one directory per role with a `ROLE.md`
- `skills/` contains the role skills used by tools
- `roles.md` summarizes the role boundaries in one place

## Discovery

The repo exposes the canonical skills through symlinks:

- `.cursor/skills/` -> `agents/skills/`
- `.codex/skills/` -> `agents/skills/`

This keeps the real content in one location while still making the skills easy for both tools to discover.
