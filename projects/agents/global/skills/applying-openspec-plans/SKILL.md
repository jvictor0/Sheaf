# Applying OpenSpec Plans

Use this workflow to plan and implement an OpenSpec change in reviewed work
units. It replaces the direct task loop in `openspec-apply-change` with detailed
planning, subagent implementation, and review.

If the user requests only a plan, stop after the plan has been reviewed. An
apply or implementation request authorizes planning and execution without an
extra approval checkpoint between them.

Use `multi-provider-code-review` by default. If the user requests single-provider
or single-reviewer review, use `single-provider-code-review` instead. Apply that
choice to plan, unit, fix, and final branch reviews unless the user limits it
to a particular phase.

The lead must read and follow the selected review skill before the first review.
That skill supplies reviewer defaults and review procedure. The sections below
supply phase-specific inputs and explain how to act on the resulting review.
Use the multi-provider skill's agreed findings or the single-provider skill's
reported findings as the inputs to fix triage; single-provider findings do not
require a second reviewer's agreement.

## Read the change and code

Select the change named by the user or clearly implied by the conversation.
If needed, use `openspec list --json`; select the sole active change, or ask
when the choice is ambiguous. Announce the selected change.

Run:

```sh
openspec status --change "<name>" --json
openspec instructions apply --change "<name>" --json
```

Read every path returned in `contextFiles`, the pending tasks, and the dynamic
apply instructions. Respect `planningHome`, `changeRoot`, and `actionContext`
edit boundaries. A workspace-planning context with no allowed edit roots is
read-only; obtain an explicit affected implementation area before editing.
Report missing required artifacts or blocked state. If all tasks are already
complete, report that state rather than inventing more work.

Treat the OpenSpec requirements and accepted design as authoritative. Inspect
the actual code, interfaces, existing tests, and repository instructions before
planning changes. Resolve implementation choices as the planner. Ask the user
only for material product or architecture choices the available requirements
cannot settle; do not send those decisions downstream to an implementer.

## Write the implementation plan

Combine the pending OpenSpec tasks into coarse work units, each with a coherent
deliverable and its own meaningful verification. Group setup, code, tests, and
documentation with the behavior they support. Avoid splitting work into tiny
mechanical steps or independent units that cannot be tested until reunited.
Units may depend on completed earlier units; make those dependencies explicit.

Create a unique temporary directory for the change. Save an index and a separate
implementation brief for each unit there. Keep plans, implementation reports,
review reports, and deferred findings out of the repository unless the user
requests otherwise. Give the user the directory path and retain it through the
final report.

The index records the ordered units, dependencies, mapping to OpenSpec task
IDs, shared contracts, and a progress table. Record the branch review base and
starting worktree state so existing user changes remain identifiable. During
execution, retain agent IDs, artifact paths, and the latest verification and
review state here so the workflow can resume after an interruption.
On resumption, read the existing index and reconcile it with the actual code,
OpenSpec checkboxes, and recorded agents before dispatching work. Continue the
existing plan rather than recreating completed units from conversation memory.

**Whenever referring to a work unit in prose, headings, dispatches, reports, or
progress updates, use `unit <X> / <total>`, for example `unit 2 / 5`.** Keep the
numbering stable. If the plan must change its unit count, update the index and
briefs and announce the new total.

Each unit's brief must be sufficient for an implementer without conversation
history. Include:

- The deliverable, mapped OpenSpec task IDs, exact acceptance criteria, and
  relevant global constraints. State what is outside this unit's scope.
- Exact files to create, modify, or delete, relevant symbols, and existing
  patterns to reuse. Confirm paths and symbols against the checkout.
- Dependencies and exact interfaces consumed and produced: names, types,
  inputs, outputs, and behavioral contracts that other units rely on.
- The chosen implementation approach and detailed pseudocode for the changed
  behavior, including concrete error handling and relevant edge cases. Resolve
  data flow, state transitions, ownership, and compatibility decisions where
  applicable. Do not leave alternatives, placeholders, or “decide as needed.”
- Specific testing requirements: cases, inputs and expected outcomes, test
  files, commands and working directories, and what passing each check proves.
  Use behavior-focused tests proportional to the change. Name other required
  checks such as types, migrations, generated assets, or documentation when
  they apply; do not manufacture a checklist for unrelated concerns.
- The files and interfaces the unit may change, verification needed before
  handing off, and the implementation report path.

Include the full applicable constraints and contracts in each brief. A link to
an earlier unit or a summary of its requirements is not a replacement for the
information this implementer needs. Resolve contradictions in the plan before
dispatch. If implementation later disproves an assumption, the controller
updates the affected briefs and coordinates the change.

## Hand off briefs by file

The lead must give every implementer and fixer the **absolute path to its
saved brief** and explicitly instruct it to read that file first and in full.
Do not summarize, paraphrase, or paste the brief into the dispatch prompt.
Keep requirements in the file so the dispatch cannot become a competing spec.

For example, an implementer dispatch includes:

```text
Implement unit 2 / 5.
Read /absolute/path/to/unit-2-brief.md first, in full. It is your implementation
specification, including the exact contracts, values, and acceptance criteria.
Work in /absolute/path/to/worktree.
Write your full implementation report to /absolute/path/to/unit-2-report.md.
Return a short status, test summary, any blocker or concern, and the report path.
```

The dispatch may add brief scene-setting and pointers to relevant artifacts;
do not paste prior units' reports or the session history. Put new decisions and
contract changes into the brief before dispatching or resuming the implementer,
and tell it to reread the updated file. Give fixers the review-file path and
the finding IDs assigned to them. Ensure every supplied file is readable from
the subagent's environment before starting it.

## Review the plan

Run the selected review skill on the complete plan. Specify in the review
brief that the artifacts under review are implementation plans: assess the proposed
code and tests against the OpenSpec context and existing code. Missing
implementation at this stage is not a defect.

Include all briefs and the index. Ask reviewers to check requirement coverage,
technical feasibility, exact contracts, dependency order, independently
testable boundaries, omissions, and ambiguity.
Save the complete review and its outcomes in the temporary directory. Resolve
actionable plan defects and re-review changed plans before execution.
Apply the review audit and convergence rules below to this phase too.

## Implement and review each unit

Use the installed `xagent-subagents` skill for dispatch, waiting, follow-up,
and session management. The lead coordinates and maintains planning artifacts;
implementation and code fixes go to subagents.

1. Dispatch a fresh implementer for each unit, alternating **Claude Opus** and
   **Codex GPT-5.5**, starting with Opus, at **high effort**. Explicit user
   choices override these defaults. Resolve supported provider model IDs and
   effort controls using the active harness; set them explicitly, including
   resumed turns. Do not silently substitute an unavailable model.
2. Record the unit's starting code state before dispatch. Capture its complete
   change range for review, including multiple commits or uncommitted changes;
   do not infer that range from `HEAD~1`. Dispatch using **Hand off briefs by
   file**. The implementer implements the chosen design,
   verifies the result, and self-reviews for omissions and unnecessary scope.
   Unresolved choices or contradictions go to the controller. Agents do not
   invent architecture or expand the assignment to resolve them.
3. Require a saved implementation report containing files changed, behavior
   implemented, commands and results, remaining concerns, and any blocker.
   Confirm the changes and evidence against the brief; an agent's completion
   message alone does not establish completion. Keep the implementer available
   for fixes.
   If it needs context or reports a blocker, address the concrete cause by
   supplying evidence or correcting its brief before resuming. Do not retry
   the same blocked assignment unchanged. Resolve implementation concerns
   against the requirements; escalate only choices the lead cannot settle.
4. Run the selected review skill on the unit's changes, supplying its complete
   brief, OpenSpec requirements, implementation report, and a fixed review
   snapshot. Select its individual-task review scope.
5. Save the complete review in a temporary file. Audit its findings as below,
   then send actionable local fixes to the same implementer. Re-run affected
   checks and review the updated code until convergence. Append rounds or
   update the report while preserving prior outcomes and the fix history.
6. Mark the corresponding OpenSpec checkboxes complete only after the work is
   implemented, verified, and its actionable local findings are resolved. If a
   task spans units, wait until every covering unit meets that condition. Keep
   any blocking or disputed requirement explicitly visible in the index.
   Resolve any acceptance criterion left unverified by the review with a
   concrete check before marking it complete.

Reuse the implementer for unit fixes while its session remains available; use
a fresh fixer using **Hand off briefs by file** if the session is lost.
New review rounds receive the previous findings and dispositions. Close retained
implementers once their unit is finished.

## Audit reviews and converge

Read and follow `responding-to-agent-review` when acting on review findings.
It governs local fixes and escalation of broader work, and routes bug
corrections through `fixing-bugs`. Give implementation and fixer subagents
explicit instructions to follow those applicable skills; a brief-file handoff
does not replace them. For plan-only findings, apply the scope guidance without
requiring a bug-reproduction test for an implementation that does not yet exist.

Before dispatching fixes from **every** plan, unit, or branch review round,
audit the proposed changes for overengineering, scope creep, and side quests.
Classify findings as local work needed for the agreed scope, disputed findings,
or deferred work. A review suggestion is not authorization to expand scope.

Save any unplanned serious refactor or broader redesign in a deferred-findings
file with the evidence, impact, why it is outside the current fix scope, and
whether it blocks the requested behavior. Report it to the user at the end.
Continue independent work where possible. If it is necessary for correctness
or a required acceptance criterion, report that part as blocked; deferring the
refactor does not make the change complete.

Carry review outcomes across rounds, including their impact on acceptance.
A lead decision to defer an actionable issue remains visible as a deferral, not
a withdrawn or fixed finding. If a finding exposes a defect in the plan,
correct the affected briefs before sending a fix; if resolving it requires
changing an accepted OpenSpec requirement, obtain the user's decision first.

Convergence means the current reviewed version has no remaining actionable
local findings and its required verification passes. Continue
fix/review rounds while they make progress. If rounds repeat the same unresolved
issue without new evidence or a viable fix, record the impasse and report the
blocker rather than cycling indefinitely or claiming success. Optional broader
improvements do not restart the implementation loop.

## Overlap work carefully

You may start the next implementer while the previous unit is being reviewed
when its prerequisites are stable. Give reviewers an immutable code snapshot
with the necessary surrounding context, such as fixed commits or a captured
source tree and diff. A saved diff alone is insufficient if reviewers also
read files that an implementer is changing. If a stable review snapshot is
unavailable, serialize review and implementation.

Avoid running a fixer and an implementer simultaneously unless their file
ownership, interfaces, generated outputs, and test resources demonstrably do
not conflict. When confidence is low, finish or pause the active writer before
dispatching the fixer. If a fix changes a later unit's dependency or contract,
update that brief and require its implementer to reconcile and verify the
change. Serialize shared Git index and branch mutations.

## Finish the change

After the units finish, run a full-branch review with the selected skill against
the recorded base and current implementation, including any uncommitted work
in scope. Supply the complete OpenSpec context and assess integration,
cross-unit behavior, requirements coverage, and the resulting design.
Include the index, prior review outcomes, and deferred-findings file so issues
set aside during individual units are considered against the completed change.

Save the review, audit every round, and dispatch high-effort Opus or GPT-5.5
fixers for actionable local issues. Give each fixer an explicit scope, relevant
brief and review paths using **Hand off briefs by file**. Batch related fixes
into coherent assignments rather than creating a fixer for every finding.
Repeat verification and full-branch
review until convergence; do not silently reduce the final check to the last
fixer's diff. Keep accepted findings, disputes, and deferred refactors visible.

Run the required final integration checks on the resulting code. Reuse valid
verification evidence when its code and dependencies are unchanged; rerun
affected checks after fixes. Confirm OpenSpec task coverage, update the index,
and close remaining agents.

Report completed units and OpenSpec tasks, verification results and limits,
unresolved disputes or blockers, every deferred serious refactor, and links to
the temporary plan and review artifacts. Distinguish completed implementation
from a change blocked on a requirement. Archiving and landing follow the user's
request and the relevant skills; they are separate from applying the plan.
