# Agents

`projects/agents` is the source of truth for shared agent guidance in this
repository.

## Layout

```text
global/
  AGENTS.md
  skills/
    <skill-id>/
      skill.yaml
      SKILL.md
sheaf/
  skills/
    <skill-id>/
      skill.yaml
      SKILL.md
vendor/
  openspec/
    VENDOR.toml
    package/          # offline OpenSpec CLI + production node_modules
```

`global/AGENTS.md` is rendered to the repository root as both `AGENTS.md` and
`CLAUDE.md`, and to user-global harness locations when global install scope is
enabled.

`global/skills/` contains skills that are safe to install into user-global
harness locations. `sheaf/skills/` contains Sheaf-only skills that render into
this repository's harness directories only.

`vendor/openspec/` holds the pinned offline-capable OpenSpec CLI. Sync it
with `make vendor-sync TOOL=openspec REF=<version>` (or
`make agents-vendor-sync` from the repo root), then install with
`make install` / `make agents-install`. Do not rely on
`npm install -g @fission-ai/openspec` for Sheaf work.

The OpenSpec `package/` tree (including production `node_modules`) is
about 21M at the current pin.

Each skill directory contains:

- `skill.yaml`: stable metadata used by the installer
- `SKILL.md`: harness-neutral source instructions

## Metadata

`skill.yaml` uses a small YAML subset:

```yaml
id: incremental-mode
name: incremental-mode
description: Follow explicit keyboard-style instructions without adjacent initiative.
targets:
  - claude
  - cursor
  - pi
  - codex
```

Supported targets are `claude`, `cursor`, `pi`, and `codex`.

Skills may target any subset of supported harnesses. For example, a Sheaf-only
Codex skill declares only:

```yaml
targets:
  - codex
```

Repo-local installs render that Sheaf skill only under `.codex/skills/`.
User-global installs render shared Codex skills only to the documented Codex
user skill locations:
`~/.agents/skills/` and `$CODEX_HOME/skills/`.

## Commands

```shell
make install
make check
make clean
make install-repo
make check-repo
make clean-repo
make install-global
make check-global
make clean-global
make vendor-sync TOOL=openspec REF=1.4.1
```

Run these from `projects/agents`, or use the root shortcuts:

```shell
make agents-install
make agents-check
make agents-clean
make agents-install-repo
make agents-check-repo
make agents-clean-repo
make agents-install-global
make agents-check-global
make agents-clean-global
make agents-vendor-sync TOOL=openspec REF=1.4.1
```

`vendor-sync` refreshes the offline tree under `vendor/openspec`.
Pass `FORCE=1` to clobber local vendor modifications.
Install still runs separately via `make agents-install`.

Default `install`, `check`, and `clean` operate on both repo-local and
user-global outputs. The `*-repo` and `*-global` targets limit the scope.
Repo-local outputs contain repository instructions and Sheaf-only skills.
Shared skills install only through agents-install-global.
Plugin-owned skills such as xagent-subagents are excluded from the agents
filesystem skill renderer (`install.py`).

## Node.js

Repo install, repo check, and `make test` / `make agents-test` require
Node.js ≥20.19 (OpenSpec's engine floor). Generation and byte-reproducible
check of vendored OpenSpec harness skills/commands both invoke the vendored
CLI entry point. Global OpenSpec CLI install skips with an explicit warning
when Node is missing or too old, and still installs other global agents
outputs; it never falls back to `npm install -g`.

## Skill Ownership

```text
Shared skills: projects/agents/global/skills -> agents-install-global
smoke-test: projects/agents/sheaf/skills/smoke-test -> agents-install-repo
xagent-subagents + launcher + runtime: plugins/xagent -> xagent-plugin-install-global
```

Install or update the xagent plugin package with:

```shell
make xagent-plugin-install-global
```

Open a new Codex conversation after installing or updating the plugin so Codex
reloads the installed skill and launcher metadata. If recovery is needed, rerun
`make xagent-plugin-install-global`. If the installed package is unmarked,
inspect it and move it aside manually rather than forcing overwrite.

User-global outputs are written to:

- `~/.claude/CLAUDE.md`
- `~/.claude/skills/<skill-id>/SKILL.md`
- `~/.cursor/AGENTS.md`
- `~/.cursor/skills/<skill-id>/SKILL.md`
- `~/.pi/AGENTS.md`
- `~/.pi/skills/<skill-id>/SKILL.md`
- `~/.agents/skills/<skill-id>/SKILL.md`
- `$CODEX_HOME/AGENTS.md`
- `$CODEX_HOME/skills/<skill-id>/SKILL.md`
- `$CODEX_HOME/hooks/sheaf/session_start_after_compact.py`
- `$CODEX_HOME/hooks.json`

`CODEX_HOME` defaults to `~/.codex`.

## Vendored tooling install

Global install also copies the vendored OpenSpec CLI into a Sheaf-managed
prefix and writes a shim:

- package: `~/.local/share/sheaf/vendor/openspec/`
- shim: `~/.local/share/sheaf/bin/openspec`

Put `~/.local/share/sheaf/bin` on your `PATH` ahead of any Homebrew or
`npm install -g` `openspec` so the managed shim wins. The installer does
not edit shell rc files. If an older global npm package remains, prefer
PATH order over deleting it; agents install never auto-removes foreign
CLIs.

### Shared Codex `$CODEX_HOME/hooks.json` ownership

The agents installer owns only its post-compaction `SessionStart` group
(matcher `^compact$`, command
`python3 <resolved-CODEX_HOME>/hooks/sheaf/session_start_after_compact.py`).
It does **not** whole-file-own `$CODEX_HOME/hooks.json`.

- Install merges that canonical group in place when present, or appends it
  after existing `SessionStart` groups, preserving unrelated groups and
  canonical xagent `PostToolUse`/`Stop` groups in relative order.
- A legacy top-level `_sheaf_agents_managed` marker is removed during
  install migration; ownership is the group matcher and command, not that
  marker.
- Check compares only the managed script and the canonical agents group;
  xagent and unrelated groups are not treated as stale agents content.
- Clean removes only the agents group and managed script, rewriting the
  shared file when foreign content remains and deleting `hooks.json` only
  when nothing non-agents remains.
- Invalid JSON or an incompatible hooks shape fails install, check, and
  clean without replacing or deleting the file. `--force` does **not**
  authorize destructive replacement of malformed shared hook configuration.
- After install or clean changes the `SessionStart` group array in a way
  that can invalidate Codex's positional trust, the installer prints a
  `/hooks` re-approval notice (symmetric with xagent's Codex trust notice).

The Codex hook injects a short developer-context reminder after compaction:
if the agent was working from a plan, checklist, or task list, it should
review that material. Codex may require reviewing and trusting the command
through `/hooks` before it runs.

`--force` still applies to other unmanaged destination files (skills,
`AGENTS.md`, and similar). To pass that flag through make:

```shell
make agents-install-global AGENTS_INSTALL_FLAGS=--force
```
