# xagent Plugin

`plugins/xagent` owns the packaged `xagent-subagents` skill, launcher, and
runtime, and it distributes them to **every harness** — Claude Code, Codex,
Cursor, and Pi. Do not install `xagent-subagents` through the shared agents
skill installer.

## Ownership

```text
Shared skills: projects/agents/global/skills -> agents-install-global
smoke-test: projects/agents/sheaf/skills/smoke-test -> agents-install-repo
xagent-subagents + launcher + runtime: plugins/xagent -> xagent-plugin-install-global
```

`projects/agents/scripts/install.py` never renders `xagent-subagents`. It still
prunes stale copies it wrote in the past, but a file carrying this plugin's
marker (`sheaf-xagent-managed: DO NOT EDIT`) is reported as `plugin-owned` and
left alone.

## What lands where

| Harness | Skill | MCP tools (`xagent_*`) |
|---|---|---|
| Codex | inside the installed plugin package | plugin `.mcp.json` |
| Claude Code | `~/.claude/skills/xagent-subagents/SKILL.md` | `~/.claude.json` → `mcpServers.xagent` |
| Cursor | `~/.cursor/skills/xagent-subagents/SKILL.md` | `~/.cursor/mcp.json` → `mcpServers.xagent` |
| Pi | `~/.pi/skills/xagent-subagents/SKILL.md` | **none — see below** |

Only the `xagent` key of each MCP registry is written; every other server the
user configured survives. An existing skill file without the plugin marker is
never overwritten — the installer fails and tells you to move it aside.

### Pi has no MCP

Pi ships without built-in MCP on purpose ("It intentionally does not include
built-in MCP, sub-agents, permission popups…" — pi `docs/usage.md`). There is
no registry to write, so a Pi controller reaches xagent through the packaged
CLI only, and cannot drive Superpowers SDD (the SDD facade is MCP-only). The
skill says so; this is a harness constraint, not a missing install step.

## Installing

```shell
make xagent-plugin-install-global
```

The plugin Make targets provision `plugins/xagent/.venv` from
`scripts/requirements.txt`, including PyYAML for the Codex plugin validator.
They use that environment for packaging, tests, and installation; no global
Python package installation is required. Install xagent's Node dependencies
with `make -C projects/xagent install` before building or installing the plugin.

After installation, open a new conversation in each harness so it reloads the
skill and MCP metadata. To confirm, ask the harness to list its tools and look
for `xagent_start_non_sdd`; on Codex you can also check `codex plugin list` reports
xagent installed and enabled at `$HOME/.agents/plugins/plugins/xagent`.

For recovery, rerun `make xagent-plugin-install-global`. If the installed
package is unmarked, inspect it and move it aside manually rather than forcing
overwrite.

## Controller stop hooks (Claude Code and Codex only)

Top-level Claude Code and Codex controllers register global `PostToolUse`
observer and `Stop` guard command hooks. Cursor and Pi are not guarded: those
harnesses get the skill/MCP (or CLI) only, with no stop-hook registration.

| Path | Role |
| --- | --- |
| `$HOME/.claude/settings.json` | Claude Code hook groups (`PostToolUse`, `Stop`) |
| `$CODEX_HOME/hooks.json` | Codex hook groups (`PostToolUse`, `Stop`) |
| `$HOME/.agents/plugins/data/xagent/controller-stop-hooks/` | Stable per-session state root (outside the replaceable plugin package) |

`CODEX_HOME` defaults to `~/.codex`. Each hook command is an absolute
`python3 …/controller_stop_hook.py --harness <claude|codex> --state-root <abs> <observe|guard>`
invocation that owns only its canonical command entry.

Behavior summary:

- One block per actionable pending-state revision: the first Stop with pending
  work returns top-level `{"decision":"block","reason":…}` naming the exact
  `xagent_await` run and cursor; an unchanged second Stop for that revision
  passes.
- Lock or malformed-state failures are fail-open: the hook exits without
  blocking and does not delete the sibling lock file.
- Reinstall is idempotent: repeated global installs leave exactly one
  canonical xagent observer group and one stop group per harness event, update
  owned commands in place, and preserve unrelated groups and their relative
  order.
- Manual rollback is operator-owned: restore from the installer's sibling
  `*.xagent-backup` (or edit the JSON) and remove or trust hooks through the
  harness UI. The installer does not ship an uninstall that rewrites trust.
- xagent-launched workers are separate top-level sessions. Native-subagent
  hook events do not mutate the parent controller's pending state; this change
  does not register `SubagentStop`.

### Codex `/hooks` trust and activation check

Codex command hooks stay inert until their commands are trusted. After install
or any effective group change that can invalidate positional trust, run
`/hooks` in Codex, approve the xagent observer and stop commands, then verify
activation in a disposable `$CODEX_HOME` (never by editing live trust blindly):

1. Dispatch `xagent_start_non_sdd` or an SDD start/follow-up and confirm a
   pending state file appears under the state root for that session.
2. First Stop blocks with the exact run id and `after_sequence`.
3. Unchanged second Stop for the same revision passes.
4. An attention/cursor advance re-arms a new blockable revision.
5. Completion or close clears pending state for that run.
6. Native-subagent PostToolUse events leave the parent session state unchanged.

## One-time SDD ledger reprovision (schema v2)

The SDD ledger was redesigned as an insert-only per-agent dispatch index at
schema version 2. There is no migration: v1 rows were already inconsistent and
their forensic content survives in the run directories they point at.

Once, before starting a service build that includes the v2 ledger:

1. Stop the xagent service.
2. Delete `<log_root>/sdd.sqlite`, `<log_root>/sdd.sqlite-wal`, and
   `<log_root>/sdd.sqlite-shm`.
3. Start the service. It provisions schema v2 on first open.

A service that finds a ledger whose `user_version` is not 2 refuses to start the
SDD manager and names the files to delete, stating that v1 data is not migrated.

**Run directories are the system of record.** Reports and submitted prompts for
SDD agents live only in `<log_root>/<run_id>/normalized.jsonl`. Any cleanup,
retention, or garbage-collection tooling must treat a run directory whose
`run_id` appears in `sdd_agents.agent_id` as evidence, not cache.
