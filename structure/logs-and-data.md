# Logs And Data

Runtime output is grouped by project at the repository root.

## Logs

Every project should write logs to:

```text
logs/<project>/
```

Log files are runtime output and are ignored by git.

Quest Runner keeps per-step transcripts beside the quest so its dashboard can
replay them: `quests/<type>/<quest>/logs/` or
`projects/<project>/quests/<type>/<quest>/logs/`. Landed experiment transcripts
live in that quest's `experiments/<number>/logs/`. These directories are local,
ignored runtime artifacts; they must never be staged, including by step commits.
Source directories named `logs` and intentional fixtures under `tests/` remain
versioned.

Git does not transport these transcripts to new clones or worktrees. Preserve
needed logs in a non-Git artifact backup before removing a checkout; restore
their quest-relative paths for dashboard replay. Experiment landing copies its
logs to the source checkout before removing its worktree. Quest state, step
commit metadata, experiment metadata, and issue archives remain versioned.

When removing previously tracked transcripts, back up the files and remove them
from the index as well as adding ignore rules. An ignore rule alone does not
untrack existing files. History rewriting is a separate, coordinated operation;
linked worktrees, snapshots, reflogs, stashes, and remote refs can retain the old
objects.

## Data

Every project should write runtime data to:

```text
data/<project>/
```

Runtime data is ignored by git. If a project needs committed fixtures or sample data, place them under that project's `tests/` or `docs/` directory and label them clearly.

## Model Assets

Large local model binaries should live under:

```text
models/
```

Model assets are runtime dependencies and are ignored by git. For example, Dictator's default STT config points to `models/ggml-base.en.bin`, which resolves to `/Users/joyo/Sheaf/models/ggml-base.en.bin` when the service runs in this repository. During the Dictator migration, the existing Whisper.cpp model was copied from `/Users/joyo/dictator/apps/dictator-main/models/ggml-base.en.bin` into that Sheaf path instead of being committed.

## Project Names

Use the project directory name from `projects/<project>/` as the `<project>` segment in `logs/<project>/`, `data/<project>/`, and `config/<project>.json`.
