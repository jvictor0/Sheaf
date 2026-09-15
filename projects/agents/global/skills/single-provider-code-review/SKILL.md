# Single-provider Code Review

Use this skill when the user requests a single-provider or single-reviewer
review. Run one native review through xagent and return its findings.

Default to **Codex Astra** (`gpt-6-astra`). Use the corresponding provider's
effort default from `multi-provider-code-review`. Explicit user provider,
model, or effort choices override the corresponding defaults; if the user
selects Claude without a model, use that skill's Claude model default.
Apply the selected settings to both the xagent session and its native reviewer.

## Review inputs and invocation

Read **Review brief** and the relevant provider subsection under **Invoking the
native reviewers** in `multi-provider-code-review`. Reuse its complete review
criteria and initial native invocation instructions. Those sections are the
source of truth for the additional checks; do not maintain a separate copy here.
This reference does not invoke that skill's two-reviewer workflow.

1. Prepare the complete brief using those criteria, including the individual
   task checks when reviewing a work unit. Supply the specification, scope,
   fixed code version, and surrounding context the reviewer needs.
2. Use `xagent-subagents` for dispatch and supervision. Launch one session on
   the selected provider, explicitly directing it to invoke that provider's
   native reviewer once with the full brief. Use only the initial invocation
   from the referenced instructions, not their cross-check or resume steps.
3. Tell the reviewer this is a single independent pass. Return evidence-backed
   findings without editing code or publishing comments. Do not dispatch a
   second reviewer, cross-check findings, or run defense/revision exchanges.
   Do not include the multi-provider prompt's promise that another reviewer
   will independently check the findings.
4. Collect the complete native review report and close the session. If the
   required provider or native reviewer is unavailable, report the incomplete
   review rather than silently substituting a different reviewer.

## Report

Present the findings with their evidence and severity, the reviewed scope, and
verification limits. Identify the reviewer used. If there are no findings, say
so. These are one reviewer's findings; do not describe them as cross-validated
or require another provider's agreement before returning them.

This skill ends after that pass. A calling implementation workflow may fix
issues and request another single pass on changed code; that is a new review
round, not a debate between reviewers.
