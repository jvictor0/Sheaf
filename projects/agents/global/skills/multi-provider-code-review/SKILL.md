# Multi-provider Code Review

Use this skill to review a code change or an individual implementation task.
Run two independent reviewers through xagent: one Codex subagent and one
Claude subagent. Each xagent session invokes its provider's review facility
with the additional review brief below, then routes cross-checks back to that
reviewer. The controller coordinates the review and records its outcomes.

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
defaults to the xagent sessions and the actual native reviewers they invoke,
including cross-checks and dispute responses. Explicit user model or effort
choices override the corresponding default. Resolve Fable's supported model
identifier in the active Claude harness; do not silently substitute another
model if it is unavailable.

1. Use the installed `xagent-subagents` skill for current dispatch, waiting,
   follow-up, and session management mechanics. Launch one Codex reviewer and
   one Claude reviewer through xagent, in parallel when available. This skill
   requires both providers regardless of which provider wrote the change.
2. Use the provider-specific invocation below. Pass the complete review brief
   to the actual native reviewer, not just its xagent parent. Preserve the
   native review instructions and add our criteria as task instructions.
   Reviewers must not recursively dispatch this two-reviewer workflow.
3. Collect both complete independent reports before sharing either with the
   other reviewer. Keep both reviewer sessions available for cross-checking
   and responses from the original finder.

If xagent, either reviewer, or a required native review skill is unavailable,
surface the blocker. Do not silently substitute a single-provider review or
report the incomplete process as a completed review.

## Invoking the native reviewers

xagent currently launches ordinary `codex exec` and `claude --print` sessions.
Selecting a provider or calling that session a reviewer does not activate its
specialized review behavior. In each xagent dispatch prompt, explicitly require
the following invocation and have the session return the native review report.

### Codex

Codex exposes its dedicated review agent through `/review` in the interactive
CLI and `codex review` / `codex exec review` non-interactively. It is not an
installed `SKILL.md` or a generic subagent named `reviewer`.

In the xagent Codex session, write the complete scope, spec references, and
additional review criteria to a prompt file. From the reviewed worktree, invoke:

```sh
codex exec review --json --model gpt-6-astra \
  -c 'model_reasoning_effort="xhigh"' - < /absolute/path/to/codex-review-prompt.md
```

The command shows the defaults; use any explicit user overrides instead.

The positional `-` reads custom review instructions from stdin. Include the
exact comparison or captured diff path in that file along with the additional
criteria. This uses custom-instructions mode; do not combine it with the
`--base`, `--commit`, or `--uncommitted` presets. Those presets select scope
when running without a custom prompt. Do not send the literal `/review` as
ordinary text to `codex exec` and assume the slash command will execute.

Retain the native review's thread ID from its JSON events and its full report.
For cross-checks and the finder's response, have the same xagent session resume
that native Codex thread with `codex exec resume <thread-id> <follow-up-prompt>`.
The follow-up supplies the other reviewer's findings or objection and the
required verdict. Preserve the original report and finding IDs.
Pass the selected model and reasoning effort on resumed turns as well.

See [Codex code review](https://learn.chatgpt.com/docs/code-review?surface=cli)
and the installed `codex exec review --help` for the review interface.

### Claude

Use Claude's `Agent` tool to invoke a named code-review subagent. Anthropic's
official `pr-review-toolkit` plugin supplies
`pr-review-toolkit:code-reviewer`; its `feature-dev` plugin also supplies
`feature-dev:code-reviewer`. Use the user's selected reviewer when specified;
otherwise use `pr-review-toolkit:code-reviewer`. Confirm that the corresponding
plugin is installed and enabled with `claude plugin list --json`; a definition
in a marketplace checkout alone does not make the agent available.

Give the xagent Claude session this dispatch instruction, replacing the brief
placeholder with the complete review brief:

```text
Invoke the Agent tool with subagent_type="pr-review-toolkit:code-reviewer".
Run the reviewer on Fable unless the user selected another Claude model.
Explicitly override the plugin agent's model setting; setting only the parent
session's model does not override the reviewer's own model declaration.
Pass the complete brief below as its prompt, keeping its native reviewer
instructions. Return its full findings and retain its agent ID so subsequent
cross-checks and responses go back to this same reviewer.

<complete review brief, including the additional criteria below>
```

For follow-ups, route the other report or objection through the same xagent
Claude session to the original native subagent using the harness's supported
resume/message mechanism. Have that subagent supply the verdict or defense.
The xagent parent must not answer on the reviewer's behalf.

This is the named review subagent, not the `code-review` plugin's separate PR
workflow or the removed Superpowers workflow. If the selected review agent is
missing, report that prerequisite rather than imitating it with a general
agent. See [Claude subagent invocation](https://code.claude.com/docs/en/subagents).

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
Close the reviewer sessions after all findings have reached an outcome.
