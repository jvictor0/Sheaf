# Capability: Agents Skill Distribution

Project: `projects/agents`
ID prefix: `asd` — requirement IDs are append-only; never renumber or reuse.

## Purpose

Maintains shared agent instructions and skills from a canonical
`projects/agents` source tree, then installs them into the harness-specific
locations used by Claude Code, Cursor, Pi, and Codex. This capability owns the
source skill metadata format, global instruction rendering, managed generated
file safety, and Makefile entry points for installing and checking the generated
agent guidance.
## Requirements
### Requirement: asd-1 — Source tree: canonical agents project
WHEN the agents project is implemented, THE repository SHALL contain `projects/agents/` with `global/AGENTS.md` as the canonical global instruction source and `global/skills/` as the canonical shared skill source tree.

#### Scenario: Source tree exists
- **WHEN** a developer inspects the repository root
- **THEN** the `projects/agents/` directory exists
- **AND** `projects/agents/global/AGENTS.md` exists
- **AND** `projects/agents/global/skills/` exists

### Requirement: asd-2 — Skill sources: one directory per shared skill
WHEN a shared skill is added, THE agents project SHALL store it under `projects/agents/global/skills/<skill-id>/` with metadata and a source `SKILL.md` body sufficient to render the skill for every supported harness target.

#### Scenario: Shared skill source exists
- **WHEN** the installer scans `projects/agents/global/skills/`
- **THEN** each shared skill is discoverable as one `projects/agents/global/skills/<skill-id>/` directory
- **AND** each shared skill includes metadata and source markdown

### Requirement: asd-3 — Initial skills: convert prompt drafts
WHEN the agents project is first implemented, THE repository SHALL include shared skills converted from the prompt drafts `about_me.me`, `git_workflow.md`, `incremental_mode.md`, `pyramid_index.md`, and `software_principles.md` found under `/Users/joyo/Desktop/prompt_draft`.

#### Scenario: Initial converted skills are present
- **WHEN** the installer scans the shared skill source tree
- **THEN** it finds source skills for `about-me`, `git-workflow`, `incremental-mode`, `pyramid-index`, and `software-principles`

### Requirement: asd-4 — Install script: repo-local target-specific skill outputs
WHEN `projects/agents/scripts/install.py install --scope repo` runs, THE installer SHALL render only Sheaf-specific skills from `projects/agents/sheaf/skills/` into the repo-local harness skill directories selected by each skill's `targets` metadata and SHALL NOT render shared skills from `projects/agents/global/skills/` into repo-local harness directories.

#### Scenario: Sheaf-only skills installed for selected repo-local targets
- **WHEN** the repo-local install command completes successfully
- **THEN** every Sheaf-only skill has an installed `SKILL.md` file in each selected repo-local harness skill directory
- **AND** the installer does not create repo-local skill outputs for harness targets omitted from the skill metadata

#### Scenario: Shared global skill is not installed repo-locally
- **WHEN** a skill exists under `projects/agents/global/skills/<skill-id>/`
- **AND** repo-local install runs successfully
- **THEN** the installer does not render that skill under `.claude/skills/`, `.cursor/skills/`, `.pi/skills/`, or `.codex/skills/`

### Requirement: asd-5 — Install script: global instruction outputs
WHEN `projects/agents/scripts/install.py install` runs, THE installer SHALL render `projects/agents/global/AGENTS.md` to repository-root `AGENTS.md` and repository-root `CLAUDE.md`.

#### Scenario: Global instructions installed
- **WHEN** the install command completes successfully
- **THEN** root `AGENTS.md` exists with the rendered global instruction content
- **AND** root `CLAUDE.md` exists with the rendered global instruction content

### Requirement: asd-6 — Install script: managed-file safety
IF an install destination exists, differs from the rendered output, and is not marked as managed by the agents installer, THEN THE installer SHALL fail without overwriting it unless the user explicitly passes a force option, except that shared `$CODEX_HOME/hooks.json` ownership SHALL be determined by the group-level contract in asd-21 and SHALL never be destructively replaced by `--force`.

#### Scenario: Unmarked conflict
- **WHEN** a destination file other than shared `$CODEX_HOME/hooks.json` exists without the managed marker
- **AND** the destination content differs from the rendered content
- **THEN** installation fails before overwriting the file

#### Scenario: Unmarked valid shared Codex configuration
- **WHEN** `$CODEX_HOME/hooks.json` is valid but carries no agents whole-file marker
- **THEN** installation applies the group-level merge in asd-21
- **AND** it does not treat unrelated valid hook content as an unmanaged whole-file conflict

### Requirement: asd-7 — Check mode: drift detection
WHEN `projects/agents/scripts/install.py check` runs, THE installer SHALL compare the rendered outputs with installed files and exit non-zero if any managed output is missing or stale.

#### Scenario: Installed output is stale
- **WHEN** an installed skill file differs from the rendered source
- **THEN** check mode exits non-zero and reports the stale path

### Requirement: asd-8 — Clean mode: managed output removal
WHEN `projects/agents/scripts/install.py clean` runs, THE installer SHALL remove only files and directories it can identify as managed agents-installer outputs.

#### Scenario: Unmanaged file exists
- **WHEN** clean mode encounters an unmarked file in a harness skill directory
- **THEN** it leaves the unmarked file unchanged

### Requirement: asd-9 — Makefile: delegated agents targets
WHEN the agents project is implemented, THE root Makefile SHALL expose delegated `agents`, `agents-install`, `agents-check`, and `agents-clean` targets that call the matching targets in `projects/agents/Makefile`.

#### Scenario: Agent install target invoked
- **WHEN** a developer runs `make agents-install`
- **THEN** make delegates to `projects/agents/Makefile`
- **AND** the agents installer runs in install mode

### Requirement: asd-10 — Install scopes: repo, global, and all
WHEN `projects/agents/scripts/install.py` runs in `install`, `check`, or `clean` mode, THE installer SHALL accept a scope option with values `repo`, `global`, and `all`, where `all` is the default and includes both repo-local and user-global outputs.

#### Scenario: Default scope is all
- **WHEN** a developer runs `projects/agents/scripts/install.py install` without a scope
- **THEN** the installer processes both repo-local outputs and user-global outputs

#### Scenario: Repo-only scope
- **WHEN** a developer runs `projects/agents/scripts/install.py install --scope repo`
- **THEN** the installer processes repo-local outputs only

#### Scenario: Global-only scope
- **WHEN** a developer runs `projects/agents/scripts/install.py install --scope global`
- **THEN** the installer processes user-global outputs only

### Requirement: asd-11 — User-global instruction outputs
WHEN `projects/agents/scripts/install.py install --scope global` runs, THE installer SHALL render `projects/agents/global/AGENTS.md` to the user-global instruction paths `$HOME/.claude/CLAUDE.md`, `$HOME/.cursor/AGENTS.md`, `$HOME/.pi/AGENTS.md`, and `$CODEX_HOME/AGENTS.md` with `CODEX_HOME` defaulting to `$HOME/.codex`.

#### Scenario: Global instructions installed
- **WHEN** the global install command completes successfully
- **THEN** `$HOME/.claude/CLAUDE.md` exists with the rendered global instruction content
- **AND** `$HOME/.cursor/AGENTS.md` exists with the rendered global instruction content
- **AND** `$HOME/.pi/AGENTS.md` exists with the rendered global instruction content
- **AND** `$CODEX_HOME/AGENTS.md` exists with the rendered global instruction content

### Requirement: asd-12 — User-global target-specific skill outputs
WHEN `projects/agents/scripts/install.py install --scope global` runs, THE installer SHALL render each shared skill into only the user-global harness skill directories selected by that skill's `targets` metadata, with `CODEX_HOME` defaulting to `$HOME/.codex`.

#### Scenario: Global skills installed for selected targets
- **WHEN** the global install command completes successfully
- **THEN** every shared skill has an installed `SKILL.md` file in each selected user-global harness skill directory
- **AND** the installer does not create user-global skill outputs for harness targets omitted from the skill metadata

#### Scenario: Codex-only global skill installed only for Codex
- **WHEN** a shared skill declares `targets` containing only `codex`
- **AND** global install runs successfully
- **THEN** the skill is rendered to `$HOME/.agents/skills/<skill-id>/SKILL.md`
- **AND** the skill is rendered to `$CODEX_HOME/skills/<skill-id>/SKILL.md`
- **AND** the skill is not rendered to `$HOME/.claude/skills/<skill-id>/SKILL.md`
- **AND** the skill is not rendered to `$HOME/.cursor/skills/<skill-id>/SKILL.md`
- **AND** the skill is not rendered to `$HOME/.pi/skills/<skill-id>/SKILL.md`

### Requirement: asd-13 — Global check mode
WHEN `projects/agents/scripts/install.py check --scope global` runs, THE installer SHALL compare rendered user-global outputs with installed user-global files and exit non-zero if any managed output is missing, stale, or blocked by an unmanaged conflicting file, while checking shared `$CODEX_HOME/hooks.json` by the group-level contract in asd-21 rather than whole-file marker or byte equality.

#### Scenario: Global output is missing
- **WHEN** a user-global skill output is missing
- **THEN** global check mode exits non-zero and reports the missing path

#### Scenario: Global output is stale
- **WHEN** a user-global instruction file differs from the rendered source
- **THEN** global check mode exits non-zero and reports the stale path

#### Scenario: Shared Codex configuration contains unrelated groups
- **WHEN** the agents-owned Codex hook group and script are current
- **AND** `$CODEX_HOME/hooks.json` also contains unrelated plugin-owned or unrelated valid groups
- **THEN** global check does not report the shared file stale solely because of those groups

### Requirement: asd-14 — Global clean mode
WHEN `projects/agents/scripts/install.py clean --scope global` runs, THE installer SHALL remove only user-global outputs marked as managed by the agents installer, except that it SHALL remove the canonical agents-owned group from shared `$CODEX_HOME/hooks.json` by the group-level contract in asd-21 while preserving all non-agents content.

#### Scenario: Unmanaged global file exists
- **WHEN** global clean mode encounters an unmarked file at a configured global destination other than shared `$CODEX_HOME/hooks.json`
- **THEN** it leaves the unmarked file unchanged

#### Scenario: Shared Codex configuration has agents-owned content
- **WHEN** global clean encounters valid `$CODEX_HOME/hooks.json` containing the canonical agents-owned group
- **THEN** it removes only that group under asd-21
- **AND** it preserves unrelated plugin-owned and unrelated valid content

### Requirement: asd-15 — Makefile: scoped agents install targets
WHEN the agents project is implemented, THE root Makefile and `projects/agents/Makefile` SHALL expose install, check, and clean targets for all scopes, repo-only scope, and global-only scope.

#### Scenario: Global install target invoked
- **WHEN** a developer runs `make agents-install-global`
- **THEN** make delegates to `projects/agents/Makefile`
- **AND** the agents installer runs in install mode with global scope

#### Scenario: Default install target invoked
- **WHEN** a developer runs `make agents-install`
- **THEN** make delegates to `projects/agents/Makefile`
- **AND** the agents installer runs in install mode with all scope

### Requirement: asd-16 — Shared skill: smoke-test

THE agents project SHALL provide a Sheaf-only `smoke-test` skill under `projects/agents/sheaf/skills/smoke-test/` (a `skill.yaml` with `id: smoke-test` targeting every supported harness plus a source `SKILL.md`) that documents how to ask production Conductor to launch services from the active worktree for human testing, replace the production-port service instances during the session, preserve git-ignored assets from the main checkout via `smoke_test: true`, and poll service health.

#### Scenario: Smoke-test skill source exists

- **WHEN** the installer scans `projects/agents/global/skills/`
- **THEN** the `smoke-test/` skill directory is not present in the global shared skill source tree
- **AND** `projects/agents/sheaf/skills/smoke-test/` is discoverable with a `skill.yaml` (`id: smoke-test`) and a `SKILL.md`

#### Scenario: Skill passes worktree to production Conductor

- **WHEN** an agent opens the `smoke-test` skill
- **THEN** the skill body explains that production Conductor handles smoke-test lifecycle requests
- **AND** it provides a copy-paste script or command sequence that verifies production Conductor is healthy, builds the current worktree services, restarts the asset-dependent services with `smoke_test: true` and a `worktree` path for the current worktree, and polls their health

#### Scenario: Skill describes worktree-code production-asset semantics

- **WHEN** an agent opens the `smoke-test` skill
- **THEN** the skill body explains that restarted services temporarily replace the production-port service instances with services launched from the worktree's code and tracked config
- **AND** it explains that git-ignored assets such as API keys, `.secrets.json`, and models continue to resolve from the main checkout asset root injected by Conductor

#### Scenario: Skill provides origin verification and recovery

- **WHEN** an agent opens the `smoke-test` skill
- **THEN** the skill body provides verification guidance that helps confirm Conductor and the restarted services are running from the worktree rather than the production checkout
- **AND** it provides recovery guidance for returning the production stack after manual smoke testing

#### Scenario: Skill distributes only to repo-local harness targets

- **WHEN** `make agents-install-repo` runs after the skill is added
- **THEN** the `smoke-test` skill is rendered into each supported repo-local harness skills location selected by its targets
- **AND** `make agents-install-global` does not render `smoke-test` into any user-global skill location

### Requirement: asd-17 — Source tree: Sheaf-only skills

WHEN a skill is specific to this repository rather than suitable for user-global distribution, THE agents project SHALL store it under `projects/agents/sheaf/skills/<skill-id>/` with the same `skill.yaml` metadata and source `SKILL.md` body format used by global shared skills, and THE installer SHALL render it only into repo-local harness skill directories.

#### Scenario: Sheaf-only skill source exists

- **WHEN** the installer scans `projects/agents/sheaf/skills/`
- **THEN** each Sheaf-only skill is discoverable as one `projects/agents/sheaf/skills/<skill-id>/` directory
- **AND** each Sheaf-only skill includes metadata and source markdown

#### Scenario: Sheaf-only skill installs to repo-local targets

- **WHEN** `projects/agents/scripts/install.py install --scope repo` runs
- **THEN** each Sheaf-only skill is rendered into the repo-local harness skill directories selected by its `targets`

#### Scenario: Sheaf-only skill is excluded from user-global targets

- **WHEN** `projects/agents/scripts/install.py install --scope global` runs
- **THEN** Sheaf-only skills are not rendered into user-global harness skill directories

### Requirement: asd-18 — Install script: obsolete global skill cleanup

WHEN a previously global skill becomes Sheaf-only or plugin-owned, THE agents installer SHALL remove or report the obsolete managed user-global outputs for that skill during global install, check, and clean operations without deleting unmanaged files.

#### Scenario: Global install prunes obsolete managed skill

- **WHEN** `projects/agents/scripts/install.py install --scope global` runs
- **AND** an obsolete user-global skill output exists with the agents managed marker
- **THEN** the installer removes that obsolete output instead of leaving it installed globally

#### Scenario: Global check reports obsolete managed skill

- **WHEN** `projects/agents/scripts/install.py check --scope global` runs
- **AND** an obsolete user-global skill output exists with the agents managed marker
- **THEN** check mode exits non-zero and reports the obsolete output

#### Scenario: Global cleanup preserves unmanaged skill

- **WHEN** `projects/agents/scripts/install.py clean --scope global` runs
- **AND** an obsolete user-global skill path exists without the agents managed marker
- **THEN** clean mode leaves that file unchanged

#### Scenario: Retired standalone skill is pruned globally

- **WHEN** global agents installation runs after `xagent-subagents` is retired
- **AND** a standalone user-global `xagent-subagents` output exists with the agents or retired plugin managed marker
- **THEN** the installer removes that obsolete standalone skill output
- **AND** it leaves unmanaged personal copies and separately installed plugin packages unchanged

### Requirement: asd-20 — Source tree: Codex global hook assets

WHEN Codex-specific user-global hook behavior is added, THE agents project SHALL store the canonical agents-owned hook-group template and hook script under `projects/agents/global/codex/`.

#### Scenario: Codex hook source assets exist

- **WHEN** a developer inspects `projects/agents/global/codex/`
- **THEN** the Codex hook-group configuration template is present in that tree
- **AND** the hook script that emits the post-compaction reminder is present in that tree

#### Scenario: Codex hook assets remain repository-owned

- **WHEN** the agents installer merges user-global Codex hook outputs
- **THEN** it uses the group template and script under `projects/agents/global/codex/` as the source of truth for agents-owned content
- **AND** it does not require hook source files that live only under `$CODEX_HOME`

### Requirement: asd-21 — User-global Codex hook installation

WHEN `projects/agents/scripts/install.py install --scope global` runs, THE agents installer SHALL install the Codex post-compaction script and merge only the `SessionStart` group identified by the canonical compact matcher and absolute installed-script command into the resolved `$CODEX_HOME/hooks.json`, preserving unrelated plugin-owned groups and unrelated valid configuration without requiring the shared JSON file to carry the agents managed marker.

#### Scenario: Global install writes Codex hook outputs

- **WHEN** the global install command completes successfully
- **THEN** the Codex hook script exists under the resolved `$CODEX_HOME`
- **AND** the Codex user-global hook configuration exists under the resolved `$CODEX_HOME`
- **AND** the hook configuration references the installed hook script with a stable absolute path
- **AND** unrelated valid top-level keys and hook groups retain their semantic values and relative order
- **AND** an absent agents-owned group is appended while an existing canonical group is updated in place

#### Scenario: Legacy whole-file marker exists

- **WHEN** valid `$CODEX_HOME/hooks.json` contains the legacy top-level `_sheaf_agents_managed` marker
- **THEN** agents installation removes that whole-file marker during the group-level merge
- **AND** unrelated hook groups are preserved during migration

#### Scenario: Global check validates agents-owned Codex hook outputs

- **WHEN** `projects/agents/scripts/install.py check --scope global` runs
- **THEN** check mode compares the rendered Codex hook script and agents-owned hook group with the installed files
- **AND** check mode exits non-zero if the script or agents-owned group is missing or stale
- **AND** unrelated plugin-owned or unrelated valid groups do not make the shared configuration stale

#### Scenario: Global clean removes only agents-owned Codex hook outputs

- **WHEN** `projects/agents/scripts/install.py clean --scope global` runs
- **THEN** clean mode removes the agents-owned hook script and agents-owned group
- **AND** clean mode rewrites the shared configuration when unrelated plugin-owned or unrelated valid content remains
- **AND** clean mode removes the configuration file only when no non-agents content remains
- **AND** clean reports that Codex `/hooks` re-approval may be required when positional group indices change

#### Scenario: Shared Codex hook JSON is malformed

- **WHEN** `$CODEX_HOME/hooks.json` is invalid JSON or has an incompatible hooks shape
- **THEN** agents install, check, and clean fail without replacing or deleting it
- **AND** `--force` does not authorize destructive replacement of malformed shared hook configuration

#### Scenario: Repo install does not activate global Codex hooks

- **WHEN** `projects/agents/scripts/install.py install --scope repo` runs
- **THEN** the installer does not write the user-global Codex hook configuration
- **AND** the installer does not write the user-global Codex hook script

### Requirement: asd-22 — Codex hook behavior: post-compaction plan reminder

WHEN Codex starts a session with `SessionStart` source `compact`, THE installed Sheaf Codex hook SHALL inject extra developer context reminding the agent to review any active plan, checklist, or task list after compaction.

#### Scenario: Hook emits reminder after compaction

- **WHEN** the hook receives a `SessionStart` event whose source is `compact`
- **THEN** the hook outputs Codex-compatible hook JSON with `hookSpecificOutput.hookEventName` set to `SessionStart`
- **AND** the hook output includes additional developer context containing the reminder "If you were working from a plan, checklist, or task list, please review it after compaction."

#### Scenario: Hook ignores non-compaction starts

- **WHEN** the hook receives a `SessionStart` event whose source is `startup`, `resume`, or `clear`
- **THEN** the hook exits successfully without injecting additional developer context

#### Scenario: Hook output identifies its source

- **WHEN** the hook injects additional developer context
- **THEN** the injected context identifies itself as a post-compaction reminder from the Sheaf Codex hook
- **AND** the injected context does not present itself as a new user request

### Requirement: asd-24 — Install script: obsolete repo-local global skill cleanup
WHEN shared skills are user-global only, THE agents installer SHALL remove or report obsolete managed repo-local outputs previously rendered from `projects/agents/global/skills/` during repo install, check, and clean operations without deleting unmanaged files.

#### Scenario: Repo install prunes obsolete managed global skill
- **WHEN** `projects/agents/scripts/install.py install --scope repo` runs
- **AND** an obsolete repo-local global skill output exists with the agents managed marker
- **THEN** the installer removes that obsolete output

#### Scenario: Repo check reports obsolete managed global skill
- **WHEN** `projects/agents/scripts/install.py check --scope repo` runs
- **AND** an obsolete repo-local global skill output exists with the agents managed marker
- **THEN** check mode exits non-zero and reports the obsolete output

#### Scenario: Repo clean removes obsolete managed global skill
- **WHEN** `projects/agents/scripts/install.py clean --scope repo` runs
- **AND** an obsolete repo-local global skill output exists with the agents managed marker
- **THEN** clean mode removes that obsolete output

#### Scenario: Repo cleanup preserves unmanaged skill
- **WHEN** a repo-local path formerly used by a global skill contains a file without the agents managed marker
- **THEN** install, check, and clean operations leave that unmanaged file unchanged

### Requirement: asd-29 — Installer: vendored OpenSpec and Superpowers integration

WHEN OpenSpec and Superpowers are vendored under `projects/agents/vendor/`, THE agents installer SHALL include their install, check, and clean outputs in the existing `repo`, `global`, and `all` scopes according to `agents-vendored-tooling` ownership (OpenSpec harness artifacts in repo scope; OpenSpec CLI and Superpowers managed plugins/packages in global scope), using `projects/agents/vendor/` as the source of truth rather than global npm or harness marketplaces.

#### Scenario: All-scope install covers vendored tooling

- **WHEN** `make agents-install` or `install --scope all` orchestration runs
  successfully on a machine with a supported Node.js
- **THEN** repo-scope OpenSpec harness outputs are installed via `install.py`
- **AND** global-scope OpenSpec CLI outputs are installed via `install.py`
- **AND** Superpowers managed plugin/package outputs are installed via
  `projects/agents/scripts/install_superpowers.py`
- **AND** existing shared-skill and instruction outputs continue to install as
  before

#### Scenario: Repo scope excludes Superpowers global outputs

- **WHEN** `projects/agents/scripts/install.py install --scope repo` runs
- **THEN** Superpowers managed plugin/package destinations are not written by
  that repo-scoped run
- **AND** OpenSpec repo-local harness outputs are still installed from vendor by
  invoking the vendored package entry point directly

#### Scenario: Global scope install.py does not write Superpowers or repo OpenSpec outputs

- **WHEN** `projects/agents/scripts/install.py install --scope global` runs
- **THEN** the managed OpenSpec CLI is installed from vendor when Node meets
  the CLI engine floor (CLI skipped with warning otherwise)
- **AND** that `install.py` run does not install Superpowers plugins (those are
  installed only via `install_superpowers.py` from Make orchestration)
- **AND** the run does not write repo-local `.claude/skills/openspec-*`,
  `.cursor/skills/openspec-*`, `.pi/skills/openspec-*`, or
  `.codex/skills/openspec-*` paths

### Requirement: asd-30 — Makefile and docs: vendored tooling entry points

WHEN vendored OpenSpec and Superpowers support is added, THE agents project SHALL document and expose Make (or script) entry points for vendor sync and for install/check/clean that include vendored tooling, and THE documentation SHALL state that Sheaf agent work must not rely on a global OpenSpec npm install or on Claude/Cursor/Codex/Pi Superpowers marketplace installs.

#### Scenario: README documents vendor-first install

- **WHEN** a developer reads `projects/agents/README.md` after this change
- **THEN** it describes `projects/agents/vendor/openspec` and
  `projects/agents/vendor/superpowers` as the source of truth
- **AND** it instructs using agents install rather than global npm OpenSpec or
  Superpowers marketplace/`pi install` setup
- **AND** it recommends disabling or removing marketplace Superpowers copies
  when using the managed packages

#### Scenario: Make still delegates agents install

- **WHEN** a developer runs `make agents-install`
- **THEN** make delegates to `projects/agents` install
- **AND** that install includes vendored OpenSpec and Superpowers outputs for
  the default scope

### Requirement: asd-31 — Installer: preserve unrelated Codex hooks
WHEN the agents installer processes valid user-global Codex hook configuration, THE installer SHALL manage only the agents-owned post-compaction hook content while preserving unrelated plugin-owned `PostToolUse` and `Stop` hook groups and unrelated valid configuration in relative order.

#### Scenario: Global install follows another plugin installation
- **WHEN** `$CODEX_HOME/hooks.json` contains unrelated plugin-owned hook groups
- **AND** `projects/agents/scripts/install.py install --scope global` runs
- **THEN** the agents installer installs or updates its post-compaction hook
- **AND** the resulting configuration retains exactly one copy of each unrelated plugin-owned hook group

#### Scenario: Global check sees valid combined configuration
- **WHEN** the agents-owned post-compaction content is current
- **AND** unrelated plugin-owned groups are present and valid
- **THEN** `projects/agents/scripts/install.py check --scope global` does not report those unrelated groups as stale agents-owned content

#### Scenario: Global clean sees plugin-owned groups
- **WHEN** `$CODEX_HOME/hooks.json` contains agents-owned and unrelated plugin-owned hook groups
- **AND** `projects/agents/scripts/install.py clean --scope global` runs
- **THEN** clean removes the agents-owned hook content
- **AND** it retains the unrelated plugin-owned groups and unrelated valid configuration

#### Scenario: Installation is repeated
- **WHEN** the agents global installer runs repeatedly with unrelated hook groups already present
- **THEN** the resulting Codex configuration contains one agents-owned post-compaction hook group
- **AND** it contains one unrelated observer group and one unrelated stop group
- **AND** the installer does not delete or reorder unrelated valid hook groups
- **AND** the installer reports the Codex `/hooks` re-approval boundary after an effective group change that can invalidate positional trust


### Requirement: asd-33 — Native CLI delegation

Implementation and review skills SHALL launch Claude agents directly with
`claude -p` and Codex agents with `codex exec`. Review skills SHALL use the
provider's native reviewer, retain their additional review criteria, and use
recorded native session IDs for follow-ups. The multi-provider review SHALL
retain its bounded cross-check and dispute protocol; single-provider review
SHALL remain a single independent pass.

#### Scenario: Resume the original Claude reviewer
- **WHEN** a finding needs a cross-check or a response from its original finder
- **THEN** the controller resumes the recorded Claude session ID with `claude -p --resume`, using the selected native review agent and model
- **AND** it saves the new report alongside prior findings

#### Scenario: Hand off an implementation unit
- **WHEN** the controller starts a unit implementer
- **THEN** its dispatch names the full brief file and requires reading it
- **AND** the controller records the native session ID for later fixes
