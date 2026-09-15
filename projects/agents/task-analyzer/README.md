# task-analyzer

SDD (spec-driven-development) data-collection and cost-model pipeline. It
mines real implementer/reviewer transcripts, scores task complexity (C1–C7)
and implementer quality (G1–G5) with cheap LLM subagents, derives per-task
dollar costs from token usage, trains a Bayesian estimator of cost per
(task complexity, model, effort) arm, and uses that estimator to help decompose
new OpenSpec changes into cost-aware task lists. See
`openspec/changes/archive/.../design.md` for the full design rationale (D1–D9)
this README assumes as background.

## Run cadence

Offline and occasional — there is no service, cron, or CI hook. A human runs
`ingest.py` after a batch of changes lands on `main` (weekly-ish is the
expected cadence, not enforced), commits the refreshed
`data/agents/task-analyzer.sqlite` + `data/agents/task-analyzer.dump.jsonl`,
and re-trains (`train.py`) only when there's enough new data to matter — there
is no fixed retrain schedule, see "Recompute matrix" below. The decomposer
(`prompts/decomposer.md`) is invoked manually per OpenSpec change, today; see
"Not yet integrated" below.

## Command reference

All commands are run from the repo root with `python3` (stdlib + numpy only,
no other dependencies). `--db` defaults to `data/agents/task-analyzer.sqlite`
where a command has a default at all; pass it explicitly when working against
a scratch/test copy.

### `ingest.py` — mechanical + agentic ingestion

```
python3 projects/agents/task-analyzer/ingest.py [ingest|rebuild-derived|backfill-turns|verify-turn-phases] \
    [--db PATH] [--repo PATH] [--dry-run] [--no-agents] \
    [--rescore complexity|grades|phase_tokens] [--change NAME] [--strict] \
    [--analysis-dir DIR] [--regenerate]
```

- `ingest` (default command): diffs landed `openspec/changes/archive/` dirs
  against git (never the worktree filesystem — a change only qualifies once
  its archive dir is present at the resolved ref, default
  `refs/heads/main`), ingests new tasks/sessions (populating `session_turns`
  for each, alongside its `sessions` row), dispatches agentic scoring for
  cache misses (complexity/grading/phase-labeling — phase-labeling also
  populates `turn_phases`), and calls `costs.rebuild` at the end. A landed
  change's tasks come from its committed SDD briefs
  (`.superpowers/sdd/<change>/*-brief.md`) when any exist at the ref; since
  `.superpowers/` is the SDD workflow's own uncommitted scratch and is
  never committed, the steady-state case is usually zero committed briefs
  — a change with none falls back to deriving tasks from its own committed
  Superpowers plan file instead (every "Task N" heading, any level, is one
  task's brief text; `task-N` keys). Committed briefs, when any exist for
  a change, always win outright over plan derivation for that change
  (followup-6; see the data-gathering spec's "Idempotent, atomic, offline
  ingestion" requirement for the full contract).
- **Session-to-task joining is evidence-based** (followup-7): a session
  joins `(change, task_key)` only when its own prompt affirmatively names
  that specific change — its change directory/OpenSpec change name, its
  committed Superpowers plan file, or its SDD brief directory path — never
  merely because it's the sole candidate left after `--change` narrowed
  the field. This evidence check runs over the FULL landed set always;
  `--change` only filters which of the resulting joins get ingested this
  run, so join/quarantine outcomes are identical scoped or unscoped. A
  session with a recognized task_key but no evidence for any change
  quarantines with reason `"no change evidence for task key"`; one with
  evidence for more than one change quarantines with `"task key matches
  >1 task"`. Every real run also heals any already-committed session
  whose `task_id` disagrees with the current evidence-based outcome
  (e.g. residue from a run predating this fix) — `RunReport.healed_sessions`/
  `healed_session_ids`, logged to `ingest_log` when nonzero. A scoped
  `--dry-run --change NAME`'s session list matches exactly what a scoped
  real run would write — never a session joined, with real evidence, to a
  change outside that scope (fix-round-1; evidence is still always
  computed against the full landed set, only *ingestion* is scoped).
- **A grading result can legitimately find nothing gradeable** (followup-7)
  — e.g. every review joined to a task turns out to be a mis-join.
  `prompts/grading.md` requires an output file for every item always, with
  a distinct "ungradeable" shape (all five grade fields null, zero severity
  counts, every excluded review listed, plus a `reason` string) for this
  case, discriminated from a real grade on the grade fields' own values —
  *never* on `reason`'s mere presence (fix-round-1: a populated grade may
  also harmlessly carry a `reason` and still ingests as a grade; a
  half-null hybrid, some grade fields null and others not, is rejected
  outright, same as any other malformed staged file). `ingest.py` records
  an ungradeable result durably WITHOUT a `grades` row
  (`RunReport.ungradeable`/`ungradeable_items`) and the run continues — if
  a `grades` row already existed for that task/rubric version (necessarily
  stale, since a fresh, different input produced this ungradeable result),
  it's deleted rather than left active (`RunReport.superseded_grades`,
  logged to `ingest_log`). A dispatch that produces no valid staged output
  at all (not even the ungradeable shape) after retry now fails only that
  one item by default (`RunReport.failed`/`failed_items`, left as an open
  gap for a future run) instead of aborting the whole run — pass
  `--strict` to restore the old abort-on-any-dispatch-failure behavior.
- `rebuild-derived`: recomputes `task_costs`/`task_arms` from already-ingested
  raw + agentic rows only — no discovery, no agent dispatch. This is the
  command to run after a price-table change (see the recompute matrix).
- `backfill-turns` (design.md D5 amendment, followup-4): for every already-
  ingested session lacking `session_turns` rows — i.e. every session ingested
  before those tables existed, which today means the entire `migrated_v0`
  corpus — re-extracts turns from `sessions.transcript_path` if that file
  still exists on disk, and (for round-0 implementer sessions that gain
  turns this way) attempts to backfill `turn_phases` too, from a still-valid
  staged `phase_labeling` JSON or else the analysis dataset's per-turn
  `phase_labels/<key>.json` (`--analysis-dir`, default
  `analysis/sdd-model-analysis/data`). Prints backfilled/unrecoverable
  counts as JSON. A session whose transcript no longer exists on disk simply
  keeps session-level cost attribution — not an error, just a documented
  precision limit (see design.md D5 amendment). Run this once after
  upgrading to schema v3, before the next `rebuild-derived`, so the
  spanning-session split has turn data to work with wherever it's
  recoverable. `turn_phases` is only ever written after verifying, per
  session, that its implied aggregation (per-turn labels summed against
  `session_turns.output_tokens`) equals the session's own `phase_tokens`
  rows EXACTLY — an all-or-nothing check, so a session whose historical
  labels no longer line up with a fresh re-extraction is left (or reset
  to) empty `turn_phases` rather than silently writing wrong per-turn
  splits; safe to re-run at any time, including to self-heal rows a prior,
  buggy backfill left behind. Refreshes `sessions.n_turns` from the
  re-extraction (many rows were stale from the original migration).
  `--regenerate` (followup-5) widens the candidate set from "sessions
  lacking `session_turns`" to EVERY session with a recoverable
  transcript, REPLACING existing `session_turns` rows rather than
  skipping them — use this once after an `extractors.py` change that
  alters per-turn token deltas (the default mode would otherwise never
  re-derive a session it already backfilled under older logic); any
  session that already has `turn_phases` rows also gets `phase_tokens`
  mechanically recomputed from those (unchanged) labels against the
  refreshed deltas.
- `verify-turn-phases`: read-only audit of the `turn_phases`/`phase_tokens`
  aggregation invariant against whatever's already committed — prints
  every violation (if any) as JSON and exits 1 if any are found, 0
  otherwise, so it's usable as a CI/pre-commit gate. Takes `--db` only.
- `--dry-run`: prints the actual computed work plan as JSON — new changes,
  new tasks, session identifiers (or, past `_DRY_RUN_SESSION_LIST_LIMIT`
  = 100, a count plus a 20-id sample instead of the full list), and every
  agentic gap with its `entity_key`/`kind`/`version`/`reason` — without
  writing anything; against a DB that doesn't exist yet, it plans against
  a throwaway in-memory schema instead of creating the file. (followup-6:
  an earlier version of this flag printed a RunReport-shaped summary
  instead — all write counts trivially zero on a run that writes nothing,
  and no plan content at all.)
- `--no-agents`: mechanical ingestion only — no xagent dispatch, no cost to
  a fleet.
- `--rescore TABLE`: opt-in re-scoring of everything whose stored
  `rubric_version`/`taxonomy_version` differs from the asset's current
  version (a rubric bump alone does not silently trigger a fleet-wide
  re-score; you ask for it).
- `--change NAME`: restricts ingestion to one change within the already-landed
  set — it never bypasses the landed check, and never changes any session's
  join/quarantine outcome, only which of them get ingested this run (see
  evidence-based joining above).
- `--strict` (followup-7): abort the whole run if any single agentic gap's
  dispatch produces no valid staged file at all, even after the built-in
  retry (the pre-followup-7 behavior). Default off — such an item now fails
  on its own and the run continues with every other task. Never triggered
  by a correctly-declined "ungradeable" grading result, which is not an
  error.
- Both commands write the refreshed `<db>.dump.jsonl` sibling next to the
  database on every real (non-dry-run) run — that JSONL file is what PRs
  should be reviewed against, since the sqlite binary itself doesn't diff
  usefully.

### `migrate_v0.py` — one-shot 2026-07-19 dataset migration

```
python3 projects/agents/task-analyzer/migrate_v0.py --db PATH \
    [--source DIR] [--reconcile-targets JSON] [--reconcile-tolerance N]
```

Already run once against the landed dataset (D8); idempotent if re-run, but
there should be no reason to run it again absent a new historical corpus to
backfill. Prints a JSON summary plus a reconciliation table (expected vs.
actual row counts against `DEFAULT_RECONCILE_TARGETS`, ±`--reconcile-tolerance`).

### `train.py` — train a new estimator generation

```
python3 projects/agents/task-analyzer/train.py --db PATH [--config JSON]
```

Reads `task_costs` joined against the *current* (numerically greatest)
`complexity` rubric version and each task's canonical `task_arms` row (tasks
with no determinable arm — no round-0 implementer session — are excluded, not
crashed on). Writes one new `estimators` row + one `estimator_params` row per
(category, arm) with data, **plus one sentinel row per category** —
`model = effort = "(pooled)"` (`model.POOLED_SENTINEL_ARM`, a value no real
provider ever reports) — carrying that category's pooled posterior (every
arm's rows in that category, fit from the weak prior); `estimate.py` falls
back to this row for any (category, arm) cell that has no posterior of its
own. `train.py` **never** touches or deletes a prior generation — old
estimators stay for audit/rollback (D9), so `estimate.py` always defaults to
the numerically greatest `estimator_id` unless `--estimator-id` pins an
older one. `--config` is a path to a JSON file of overrides deep-merged onto
`model.default_config()` (feature list, epsilon, prior hyperparameters,
`min_rows_per_arm`).

### `estimate.py` — score a candidate decomposition

```
python3 projects/agents/task-analyzer/estimate.py \
    (--decomposition FILE | --sanity) [--db PATH] \
    [--estimator-id N] [--seed N] [--mc-draws N] \
    [--p20] [--json]
```

Read-only against the database — never writes to it. `--decomposition` takes
either a `.json` file or an annotation-subset `.yaml` (same format
`annotations.py` reads/writes); `--sanity` scores a fixed synthetic 3-task
reference decomposition at composites {2, 3, 4} instead, for smoke-checking a
freshly trained estimator without hand-writing an input file. `--json` emits
the full machine-readable report; omit it for the human table (arm rankings,
selected-arm marker, `unscorable_arms`, decomposition totals).

An arm missing a per-arm posterior for some category falls back to that
category's pooled posterior (the `"(pooled)"` sentinel row `train.py`
persists — see above); each arm's `fallback_categories` in the report lists
which categories, if any, resolved that way (diagnostic only — it does not
by itself change selection). An arm is `unscorable` only if even the pooled
fallback has no row for a category it needs.

A task's total cost per arm is the *sum* of independent per-category
predictives — and quantiles don't commute with sums in general (for
moderate-tailed posteriors, summing each category's p80 typically
overstates the total's true p80 and summing each category's p20 typically
understates it, though that direction isn't guaranteed at very heavy tails
— see `estimate.py`'s module docstring), so totals are computed by seeded
Monte Carlo (`--seed`, default 0; `--mc-draws`, default 2000) rather than
as a sum of each category's own quantile. Non-finite draws (an overflowed
`exp` on a sufficiently heavy-tailed posterior) are excluded before
quantiles are taken; an arm with zero finite draws is `unscorable`
(`reason: "all_draws_nonfinite"`), and no report ever contains an
`Infinity`/`NaN` value. Selection defaults to **Thompson sampling**: one
posterior draw per arm (summed across categories), argmin among all scorable
arms with a finite total. `--p20` selects the arm with the lowest MC
`p20_total_usd` instead. `p80_total_usd` is reported for budgeting and never
excludes an arm in either mode. Thompson draws come from a rng stream
independent of the MC draws (both derived from `--seed` via
`numpy.random.SeedSequence.spawn(2)`), so a report's MC
`p20`/`p50`/`p80` totals are identical between modes. Both modes are fully
deterministic given the same `--seed`/`--mc-draws`/`--estimator-id`.

### `annotations.py` — validate a sibling annotation file

```
python3 projects/agents/task-analyzer/annotations.py FILE \
    (--plan PLAN.md | --tasks task-1,task-2) [--db PATH] [--estimator-id N]
```

Validates format version, task-key coverage against the plan as a *multiset*
(unknown/missing/duplicate all reported separately), C1–C7 ranges, composite
agreement, and `(model, effort)` arm membership (only checked if `--db` is
given — omitting it skips arm validation but still runs everything else).
Prints every violation, then an error count, and exits nonzero if any;
prints `valid` and exits 0 otherwise.

## Recompute matrix

| Change | What to run | Why |
| --- | --- | --- |
| Complexity/grading/phase-taxonomy rubric bump (frontmatter `version:` in `rubrics/*.md`) | `ingest.py --rescore <table>` | Re-scores every stored judgment whose recorded version differs from the asset's new one; old-version rows are kept (audit trail), not overwritten in place. |
| `model_prices` price-table change | `ingest.py rebuild-derived` | Recomputes `task_costs`/`task_arms` from already-ingested raw + agentic rows using the new price rows — cheap, no re-ingestion or re-scoring needed. |
| Phase taxonomy bump (new/renamed phase keys) | `ingest.py --rescore phase_tokens`, then relabel/remap any downstream category naming assumptions by hand (the taxonomy's phase keys feed `task_costs.category` via D5's event rules) | A taxonomy version bump changes what a "phase" *is*, not just its price — treat it as a relabel, not a mechanical recompute; review before retraining. |
| New batch of ingested changes | `ingest.py`, then eyeball `--dry-run` first if the batch is large or new-provider sessions are involved | Normal cadence — no forced retrain; training is a separate, deliberate step. |
| Estimator retrain | `train.py --db data/agents/task-analyzer.sqlite`, then `estimate.py --sanity` as a smoke check | No fixed cadence — retrain when there's enough new `task_costs` data to matter (a handful of new tasks won't move sparse-arm posteriors much). Every retrain adds a new `estimators` row; nothing is overwritten, so a bad retrain is recoverable by pinning `--estimator-id` back to the prior generation. |
| Schema upgraded to v3 (`session_turns`/`turn_phases` added) | `ingest.py backfill-turns`, then `ingest.py rebuild-derived` | One-time, per-DB: populates per-turn data for sessions ingested before v3 (schema migration itself is automatic and idempotent on every `db.connect()` — see design.md D2 — but per-turn *data* can only come from re-reading transcripts, hence the separate explicit step); `rebuild-derived` then lets the spanning-session split (D5 amendment) use it. Safe to skip — sessions without turn data just keep the old, session-level (unsplit) apportionment. |
| `extractors.py` per-turn delta logic changed (e.g. followup-5's silent-checkpoint fix) | `ingest.py backfill-turns --regenerate`, then `ingest.py rebuild-derived`, then `train.py` + `estimate.py --sanity` | Per-turn token deltas for ALREADY-backfilled sessions are stale until re-derived — the default (non-`--regenerate`) `backfill-turns` only fills gaps, it won't touch a session that already has `session_turns`. `--regenerate` also mechanically recomputes `phase_tokens` for sessions with existing `turn_phases`, so the aggregation invariant (`verify-turn-phases`) holds afterward. |

## Persistent agents and the spanning-session split

The retired `openspec-superpowers-workflow` skill kept implementer and
reviewer sessions **open and resumed** across fix/re-review rounds. Those
historical sessions broke a session-granularity
assumption the original cost model (D5) depended on: a "round-0" implementer
session can, in practice, still contain turns performed *after* the task's
first review verdict — pure session-level apportionment would misattribute
that fix work to the phase categories instead of `followup_fix`. Likewise, a
resumed reviewer session doing review + re-review is still one session row,
so counting "one review boundary per reviewer session" undercounted rounds
for anything that started between two verdicts.

The fix (design.md D5 amendment, followup-4) is per-turn, not per-session:
every verdict is detected mechanically inside the transcript (a `SPEC:
PASS|FAIL` + `QUALITY: ...` co-occurrence on the same turn's assistant
text), review boundaries become the union of every detected verdict turn
(not one per reviewer session), and `costs.py` splits a spanning round-0
session's *turns* at the task's first boundary — pre-boundary turns fund the
phase categories, post-boundary turns fund `followup_fix` — rather than
assuming a session belongs entirely to one category. This needs per-turn
timing/labels (`session_turns`/`turn_phases`, schema v3) that didn't exist
before; `ingest.py backfill-turns` populates them for already-ingested
sessions where the source transcript is still recoverable (see the recompute
matrix row above and design.md D5's amendment section for the exact fallback
rules when it isn't). Verdict detection requires a standalone, single-valued
`SPEC:`/`QUALITY:` line near the end of the message — never a line that
names both alternatives (every review brief's own instructions quote the
format that way, e.g. `SPEC: PASS or SPEC: FAIL`), so a reviewer restating
its brief is never mistaken for having rendered a verdict; a genuine
verdict's own trailing one-line reason (including one with a backtick-quoted
code identifier or the ordinary word "or") still matches.

**Resolved: `sessions.output_tokens` vs. `session_turns` sum (followup-5).**
Earlier revisions of this section documented a real gap where the
session-level `output_tokens` total and the sum of that same session's
`session_turns.output_tokens` disagreed — for a real fraction of sessions
(roughly 60% of the migrated corpus), sometimes by a lot, biasing the
spanning-session split's `followup_fix`/phase category attribution (not
just the diagnostic `weighted_tokens` figure — see fix-round-2's report
section for the full analysis before this was fixed). Root cause: a
token-usage checkpoint (a codex `token_count` event, or a claude assistant
message) whose corresponding content produced zero condensed timeline items
(no `SAY:`/`CALL:`/`OUT:`/`THINK:` line) was folded into the session-level
running total but never became its own `Turn`, so its delta silently
dropped out of the per-turn sum.

Fixed in `extractors.py`: every such "silent checkpoint" delta is now
folded into an EXISTING adjacent turn instead of discarded — mass before
the first visible turn folds into turn 1, a gap between two visible turns
folds into the next one, and trailing mass after the last turn folds into
that last turn. Turn count and indices are never changed by this (they're
the join key for `turn_phases` and the rendered timelines). For codex
specifically, each checkpoint's delta is now computed from the increase in
the cumulative `total_token_usage` counter since the previous checkpoint
(verified monotonically non-decreasing across the whole real corpus)
rather than trusted from `last_token_usage` directly — this also correctly
attributes a resumed/continued (`thread_spawn`) session's inherited
leading baseline (its file can begin with a large nonzero cumulative total
already present at the very first checkpoint, reflecting activity from
before this file's own recorded events start).

Post-fix invariant: `sum(session_turns.output_tokens) ==
sessions.output_tokens` for every session with at least one turn —
verified with zero exceptions across the entire real committed corpus. A
session with genuinely zero turns (nothing to fold pending mass into) is
an explicit, tested exception, not a silent violation of a well-formed
transcript's reconciliation. `costs.rebuild`'s turn-coverage diagnostic
(`RebuildResult.spanning_session_coverage`, plus a stderr `WARN` below
`costs.DEFAULT_COVERAGE_WARN_THRESHOLD = 0.9` — see fix-round-2's report
section for how this was added) should now read `1.0` for every spanning
session; see `.superpowers/sdd/task-analyzer/followup-5-report.md` for the
real dataset's per-session before/after coverage numbers and the
quantified `followup_fix` shift this produced.

Already-ingested sessions need their `session_turns` (and any dependent
`phase_tokens`) regenerated under the fixed extractor to actually benefit
from this — `ingest.py backfill-turns --regenerate` does this (see the
command reference above): it replaces `session_turns` for every session
with a recoverable transcript (not just ones lacking `session_turns`), and
for any session that already has `turn_phases` rows, mechanically
recomputes `phase_tokens.output_tokens`/`.turns` from those (unchanged)
phase labels joined against the refreshed deltas — phase labels are the
agentic judgment and are never re-derived by this step, only their token
weight.

## Staging / crash-recovery semantics

- Each task is ingested inside **one SQLite transaction** (task row +
  sessions + any agentic results, or nothing) — a crash mid-task leaves no
  partial row, just a task that will be picked up again on the next run.
- Agentic dispatch (complexity/grading/phase-labeling) writes its outputs to
  a staging directory first, atomically (tmp file + rename), keyed by
  `staging/<kind>/<entity_key>__v<version>__<input_sha256[:12]>.json`. A
  re-run after a crash consults staging by exact filename before
  re-dispatching anything — an interrupted run does not re-spend fleet
  tokens on work it already produced, it just re-reads the staged file.
  Files that fail schema validation on read-back are never upserted or
  treated as complete (see `agents.py`'s validate-then-write-atomically
  contract).
- `--dry-run` never writes to the target database (and never creates one
  that doesn't already exist — it plans against an in-memory schema
  instead), so it's always safe to run after a crash to see what the next
  real run would do before committing to it.

## Not yet integrated

The decomposer (`prompts/decomposer.md`) is usable today only via **manual
subagent dispatch** — there is no automated hook into a planning flow. Wiring the
decomposer into a planning workflow (so a plan's task list and its
`.assignments.yaml` sibling are generated together, instead of the
decomposer being invoked by hand against an already-written plan) is
explicitly deferred to a future change (design.md's Migration Plan, step 3).
See `projects/agents/task-analyzer/examples/` for a worked dry-run of manual
dispatch against an already-archived change.

## Configuration

- `TASK_ANALYZER_XAGENT_MAIN` (env var): overrides the resolved path to the
  xagent CLI entry point (`node <path> run ...`) that `agents.xagent_runner`
  shells out to for real agentic dispatch. Resolution order: the
  `xagent_main` keyword argument passed directly to `xagent_runner` (highest
  priority) → this env var → the repo-relative default
  (`projects/xagent/dist/src/main.js`). Set it when running the pipeline
  against a different xagent checkout/build (e.g. a worktree with its own
  `dist/`); a missing resolved path raises loudly (`RuntimeError` naming all
  three resolution sources) rather than silently producing zero staged
  files.

## Known gaps / TODOs

- **Price audit**: `data/agents/task-analyzer.sqlite`'s `model_prices` table
  was populated during migration (D8) from the 2026-07-19 dataset's
  point-in-time price assumptions; ten rows (the eight gpt-5.x models plus
  claude-sonnet-4-6 and claude-fable-5) were $1/$1/$1 placeholders until a
  follow-up price audit (2026-07-23, see `schema.sql`'s seed comment)
  replaced them with verified list prices and retrained the estimator. No
  row here is independently re-audited on a schedule, though — before
  relying on `estimate.py`'s dollar figures for a real budget decision (as
  opposed to the relative/directional comparisons the decomposer uses),
  re-check `model_prices` against each provider's current pricing pages and,
  if it's stale, add a new `price_version` row and run
  `ingest.py rebuild-derived` to repropagate it into `task_costs`.

## Tests

```
python3 -m unittest discover -s projects/agents/task-analyzer/tests -v
```
