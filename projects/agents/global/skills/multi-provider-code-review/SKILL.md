# Multi-provider Code Review

Use this skill to review a code change or an individual implementation task.
Run two independent reviewers using their native CLIs: one Codex review agent
and one Claude review agent launched with `claude -p`. Add the review brief
below to each provider's native review instructions. The controller coordinates
cross-checks and records outcomes.

## Review brief

Give both reviewers the same complete brief: the change being reviewed, its
base and head or captured working-tree diff, relevant specification and
acceptance criteria, and any task boundaries. Include enough surrounding code
and test context to assess the change. Keep the reviewed code fixed through
cross-checking so both reviewers assess the same version.

Both reviews must assess:

- **Correctness:** Does the code behave correctly, including relevant edge
  cases, failure paths, and interactions with existing behavior? Do the tests
  substantiate that behavior?
- **Specification compliance:** Does the implementation satisfy the spec and
  acceptance criteria? Identify missing behavior, contradictions, and additions
  outside the intended scope. If no spec is available, use the supplied user
  requirements and state the limit; do not invent requirements.
- **Simplicity:** Could the same requirements be met more simply? Look for
  overengineering, speculative abstractions, and unnecessary machinery.
  Explain the concrete simplification and its benefit rather than treating
  personal style preferences as defects.
- **Unnecessary defensive code and restrictions:** Look for complex guards,
  fallbacks, or client-side pre-validation without a concrete benefit, and
  restrictions that exclude legitimate behavior. Apply `software-principles`
  when judging their justification. Consider removing unnecessary defenses
  rather than elaborating them. A finding requesting more defensive code must
  identify the requirement or credible failure mode, explain why existing
  handling is insufficient, and account for valid uses the restriction would
  exclude. “Safer” or “more defensive” alone is not a justification.

For an **individual task review**, also inspect how the task fits into the
surrounding code: duplication, awkward factoring or responsibility boundaries,
emergent design across accumulated tasks, and unnecessary complexity. Consider
whether the growing implementation still forms a coherent design, whether
existing code should be reused, and whether a proposed abstraction earns its
cost. Account for the task's scope and dependencies when judging completeness.

Ask for findings with stable IDs, severity, file/line references, the concrete
problem and supporting evidence, and the relevant requirement or simpler
alternative. Require uncertainty to be explicit. Reviews produce findings;
they do not modify the implementation.

## Independent reviews

Unless the user specifies otherwise, use **Astra at extra-high reasoning
effort** (`gpt-6-astra`, `xhigh`) for Codex and **Fable** for Claude. Apply these
defaults to the actual native reviewer sessions, including cross-checks and
dispute responses. Explicit user model or effort
choices override the corresponding default. Resolve Fable's supported model
identifier in the active Claude harness; do not silently substitute another
model if it is unavailable.

1. Launch one Codex reviewer and one Claude reviewer directly from the
   reviewed worktree, in parallel when available, using the commands below.
   This skill requires both providers regardless of who wrote the change.
2. Pass the complete brief to each native reviewer. Preserve its native review
   instructions and add our criteria as task instructions. Reviewers must not
   recursively dispatch this two-reviewer workflow.
3. Save each process's output, exit status, full report, and native session ID
   in the review's temporary directory. Collect both independent reports before
   sharing either with the other reviewer. Retain the session IDs for resuming
   the original reviewers during cross-checks and dispute responses.

If either CLI, reviewer model, or required native review agent is unavailable,
surface the blocker. Do not silently substitute a single-provider review or
report an incomplete process as completed. Follow the running process through
completion using the host's process tools; avoid repeated short status polls.
A process exit is not sufficient: inspect its result for errors and confirm
that it produced the requested review.

## Invoking the native reviewers

Launch these commands directly; no service or wrapper session is required.
Use absolute paths for prompt and result files, quote them in shell commands,
and set the working directory to the reviewed worktree or fixed snapshot.
Keep separate output files for each reviewer and round. Do not disable session
persistence: later turns must resume the original reviewer. Supply permissions
appropriate to the assigned work through the normal CLI/host controls; surface
permission failures rather than bypassing them.

### Codex

Codex exposes its dedicated review agent through `/review` in the interactive
CLI and `codex review` / `codex exec review` non-interactively. It is not an
installed `SKILL.md` or a generic subagent named `reviewer`.

Write the complete scope, spec references, and additional review criteria to
a prompt file. From the reviewed worktree, invoke:

```sh
codex exec review --json --model gpt-6-astra \
  -c 'model_reasoning_effort="xhigh"' - < "/absolute/path/to/codex-review-prompt.md" \
  > "/absolute/path/to/codex-review-events.jsonl"
```

The command shows the defaults; use any explicit user overrides instead.

The positional `-` reads custom review instructions from stdin. Include the
exact comparison or captured diff path in that file along with the additional
criteria. This uses custom-instructions mode; do not combine it with the
`--base`, `--commit`, or `--uncommitted` presets. Those presets select scope
when running without a custom prompt. Do not send the literal `/review` as
ordinary text to `codex exec` and assume the slash command will execute.

Retain the native review's thread ID from its JSON events and its full report.
For cross-checks and the finder's response, resume that native Codex thread
with `codex exec resume <thread-id> -`, supplying the follow-up file on stdin.
The follow-up supplies the other reviewer's findings or objection and the
required verdict. Preserve the original report and finding IDs.
Pass the selected model and reasoning effort on resumed turns as well.

See [Codex code review](https://learn.chatgpt.com/docs/code-review?surface=cli)
and the installed `codex exec review --help` for the review interface.

### Claude

Anthropic's official `pr-review-toolkit` plugin supplies the native
`pr-review-toolkit:code-reviewer` agent; `feature-dev` also supplies
`feature-dev:code-reviewer`. Use the user's selected reviewer when specified;
otherwise use `pr-review-toolkit:code-reviewer`. Confirm the corresponding
plugin is installed and enabled with `claude plugin list --json`.

Run the named native agent directly as a headless session with `--agent`:

```sh
claude -p --agent pr-review-toolkit:code-reviewer --model fable \
  --output-format json < "/absolute/path/to/claude-review-prompt.md" \
  > "/absolute/path/to/claude-review-result.json"
```

The prompt file contains the full review brief and additional criteria.
`--agent` loads the plugin's reviewer instructions; a general session merely
called a reviewer does not. Set the model explicitly, using any user override,
and confirm the session uses the selected agent and model. Do not use `--bare`,
which skips normal plugin discovery, or `--no-session-persistence`.

Save the JSON result's `session_id` and complete report. For cross-checks and
the finder's response, resume this exact reviewer with a new prompt file:

```sh
claude -p --resume "<session-id>" --agent pr-review-toolkit:code-reviewer \
  --model fable --output-format json \
  < "/absolute/path/to/claude-follow-up.md" \
  > "/absolute/path/to/claude-follow-up-result.json"
```

Preserve the selected model, any explicit effort override, original finding
IDs, and prior reports across resumed turns. Check that the native reviewer
is still available before resuming; do not accept a fallback to a general
session if the plugin is missing.

See [Claude native agents](https://code.claude.com/docs/en/sub-agents) and
[headless sessions and resuming](https://code.claude.com/docs/en/headless).

### Additional prompt language

Append this to both native reviewers' task prompts, together with the full
scope, requirements, and reporting instructions from **Review brief**:

```text
In addition to your native review criteria, assess whether the code is correct,
matches the supplied spec and acceptance criteria, and achieves the requirements
in a simple way. Look for concrete opportunities to avoid overengineering.

For an individual task review, also check code duplication, awkward factoring,
the design emerging across accumulated tasks, and unnecessary complexity. Read
the surrounding code needed to judge reuse and responsibility boundaries.

Report evidence-backed issues with file/line references and explain either the
violated requirement or the concrete simpler alternative and its benefit.
Make uncertainty explicit. Return findings only; do not edit code or publish
review comments. Another reviewer will independently check your findings.
```

## Cross-check and resolve

Send each complete review to the other reviewer. For every finding, require
an explicit **agree** or **dispute** verdict supported by code, spec, or test
evidence. Silence or omission is not agreement. Preserve the original finder
and stable finding ID throughout; link duplicates and report an agreed issue
only once.

Process each finding independently:

| Response | Outcome / next step |
| --- | --- |
| The other reviewer agrees with the initial finding | Record it as a final review finding. |
| The other reviewer disputes the initial finding | Send the finding and the complete objection back to its original finder. Ask the finder to withdraw, defend, or revise it. |
| The original finder agrees the finding is wrong and withdraws it | Discard it from the final findings. |
| The original finder defends the finding | Record it as a disputed finding, including the objection and defense. End discussion of that finding. |
| The original finder revises the finding | Send the revised finding to the other reviewer for one final agree/dispute check. |
| The other reviewer agrees with the revised finding | Record the revised version as a final review finding. |
| The other reviewer disputes the revised finding | Record the revised version as a disputed finding, with both positions. End discussion of that finding. |

There is at most one response from the original finder and, if revised, one
additional cross-check per finding. Do not restart the exchange, relabel a
disputed finding to obtain another round, or let the controller silently
overrule a dispute. A substantively new issue discovered during cross-checking
gets its own finding ID and the same bounded process.

## Final report

Present **final review findings** and **disputed findings** separately, ordered
by severity within each group. Final findings contain the version both
reviewers agreed on. Disputed findings include the current claim, original
finder, other reviewer's objection, and the finder's defense or revision so
the user can judge the disagreement. Withdrawn findings are not final findings.

State the reviewed scope and any verification limits or incomplete checks.
If there are no agreed findings, say so while still reporting any disputes.
Finish any running reviewer processes after all findings have reached an
outcome. Retain reports and session IDs with the review artifacts; completed
CLI processes need not stay running between turns.
