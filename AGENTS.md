# Sheaf Agent Roles

This repository keeps its agent role and skill definitions in `agents/`.

Tool-specific skill discovery paths are thin wrappers:

- Cursor project skills live in `.cursor/skills/`
- Codex project skills live in `.codex/skills/`

Those tool-specific directories should point back to the canonical role skills in `agents/skills/`.

## Roles

- `planner`: interactive spec authoring and quest creation
- `implementer`: spec-driven implementation with no spec edits
- `reviewer`: diff/spec/code review and issue tracking
- `polisher`: fixes reviewer findings and only changes specs when redesign is required
- `committer`: final process owner, side quest creation, and completion handling

## References

- `agents/README.md`
- `docs/workflow/README.md`
- `docs/workflow/quests.md`
- `docs/workflow/stages.md`
- `docs/workflow/issues.md`
