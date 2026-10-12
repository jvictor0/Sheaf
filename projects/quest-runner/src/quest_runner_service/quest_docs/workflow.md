# Quest Workflow Reference

## Workflow Routing

The canonical recursive runner resolves runnable work from the active workflow
definition. Machine states declare `run.profile`, and the selected profile supplies
the harness, prompt, runtime-context toggles, task template, modify rules, and thread
templates. The packaged default workflow lives under `default_workflow/`.

The runner reuses one thread per profile scope:

- quest-scoped profiles: `{repo}_quest_{quest_number:04d}_{profile}`
- child-scoped profiles: `{repo}_quest_{quest_number:04d}_slice_{child_number:04d}_{profile}`

## Prompt Contract

Every profile message should make the following explicit:

- current quest
- current profile
- active child path when the profile exposes one
- path to this quest reference directory when the profile exposes one
- current experiment id when running in an experiment worktree

The first message in a profile thread includes:

1. the profile prompt from the workflow profile
2. workflow-rendered runtime context
3. the current task instruction

Later messages reuse the same thread and include:

1. workflow-rendered runtime context
2. the current task instruction

## Ownership Rules

- `physical_planner` writes slice plans, not code.
- `physical_planner` must fully plan all work explicitly required by the quest spec.
- `physical_planner` must not leave stubs or placeholder planning for
  spec-defined work.
- `physical_planner` must escalate to a human if further research is required to
  complete the plan.
- On the initial `PhysicalPlanning` pass, `physical_planner` decides the full ordered
  slice list, initializes slice scaffolding with `scripts/quest-runner slices init`,
  then writes the slice plans from the quest specs.
- `physical_planner` appends to quest-root `physicalplan_issue_responses.md` when
  addressing open physical plan issues; only `physical_planner` may write that file.
- When `ReviewPhysicalPlan` sends the quest back to `PhysicalPlanning`, the planner
  should address the open review issues, update the plans, and record per-issue
  responses in `physicalplan_issue_responses.md` rather than receiving the initial
  planning task again.
- `physical_plan_reviewer` writes only quest-level `physicalplan_issues.md` and must
  read `physicalplan_issue_responses.md` when verifying planner follow-up; reviewers
  must never write to `physicalplan_issue_responses.md`. When the issue list is clear
  and the review is accepted, it creates `physicalplan_accepted.md`.
- `implementer` writes code/tests for the current slice and creates
  `implementation_done.md` when complete.
- `polisher_reviewer` writes only slice-level `polishing_issues.md` and must read
  `slices/<slice>/polishing_issue_responses.md` when verifying polisher follow-up;
  reviewers must never write to `polishing_issue_responses.md`. When the issue list is
  clear and the review is accepted, it creates `implementation_accepted.md`.
- `polisher` fixes open polishing issues but does not close them; only `polisher` may
  append to `slices/<slice>/polishing_issue_responses.md` for that slice.
- `integration_tester` writes and runs quest-level integration tests after all slices
  are complete. It creates and closes issues in `integration_test_issues.md`, but does
  not fix the bugs it finds.
- `integration_test_polisher` fixes open integration test issues but does not close
  them; only `integration_test_polisher` may append to
  `integration_test_issue_responses.md`.
- `documenter` writes only the current project's `docs/` directory, normally
  `projects/<project>/docs/`.

Normative format for issue response files: `docs/quest/schemas/issue-responses.md`.

## Issue Lifecycle

- Reviewers create and close issues in reviewer-owned issue files; statuses remain only
  `open` or `completed`.
- Responders record how they addressed each open issue they touch by appending sections
  to the matching issue responses file (`physicalplan_issue_responses.md` or
  `polishing_issue_responses.md` or `integration_test_issue_responses.md`); they must
  not mark reviewer-owned issues `completed`.
- Reviewers read issue responses when re-checking open issues; they must not edit
  response files—if a response is inadequate, they update the issue and/or escalate.
- If an issue stays `open` across more than one cycle after the responder had a chance
  to reply, the reviewer enriches the issue with actionable detail (what was checked,
  what is still wrong, what would close it).
- If reviewer and responder positions remain in conflict after one more focused update
  cycle (including `Fixed` vs reviewer-disagrees or `NotFixed` due to scope/spec
  dispute), the reviewer escalates via quest-root `human_intervention_request.md`.
- Non-reviewer roles may add implementation notes or fixes, but must not mark
  reviewer-owned issues `completed`.
- If work cannot proceed without a major decision or clarification, write
  `human_intervention_request.md` and stop.

## Slice Execution Loop

1. Runner selects the current role from quest state and slice state.
2. Runner sends the role prompt and task.
3. Implementer always receives an additional follow-up asking it to create
   `implementation_done.md` if the slice is complete.
4. Reviewers receive an additional follow-up only when their issue list is clear,
   asking them to create `physicalplan_accepted.md` or `implementation_accepted.md`.
5. Runner evaluates file-based predicates and issue-file state.
6. Runner records each successful top-level step in git (one commit per step, with
   recursive metadata in the commit message). Legacy quests may still have
   `state_history.md` entries from older runners, and dashboard readers merge both
   history sources.

## Experiment workflow

Experiments replay a completed quest from an earlier step using a separate
worktree. Metadata and landed archives live under
`<quest_dir>/experiments/<number>/` on the source checkout.

### Agent CLI requirement

When runtime context includes an experiment id, every Quest Runner CLI call from
that agent must pass `--experiment-id <id>`. The original quest worktree may not
exist. Commands that omit the experiment id fail when only the experiment worktree
is available.

### Experiment completion

When the configured stop condition is reached, the quest filesystem state becomes
`ExperimentComplete` and source metadata status becomes `experiment_complete`.
This is not normal quest landing. Operators land experiments with
`scripts/quest-runner experiments land` or `POST /experiments/land` after review.

### Archived experiment artifacts

After landing, JSONL logs copy to `experiments/<number>/logs/`. These are ignored
local replay artifacts, as are `<quest_dir>/logs/`; Git commits and pushes do not
include them. Back them up separately before deleting the source checkout.
Restoring the same paths makes them available to the dashboard again. Issues and issue
responses copy to `experiments/<number>/issues/` and
`experiments/<number>/issue_responses/` with quest-relative paths preserved.
