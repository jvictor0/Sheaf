# Responding to Agent Review

Use this skill when acting on findings from an agent review. Read the findings
with the relevant code and requirements before deciding what to change. A
reviewer's suggestion needs technical support and does not itself expand the
user's requested scope.

For reviews conducted through `multi-provider-code-review`, use that skill for
the review procedure and finding outcomes. This skill governs what work to do
in response; it does not replace that process.

## Fix local correctness issues

Fix verified issues within the current scope, such as incorrect behavior,
missing required cases, broken contracts, or tests that fail to detect the
reported defect. Use `fixing-bugs` for bug corrections, including its required
reproduction and explanation before implementation.

Make fixes fit naturally into the component's existing responsibilities and
design. Prefer a clear local correction over an accumulating set of special
cases. Preserve surrounding behavior and avoid incidental cleanup. Follow the
active implementation workflow for who performs fixes and how they are reviewed.

## Evaluate defensive changes

Before accepting a request for additional validation or defensive code, apply
`software-principles`. Require a concrete requirement or credible failure mode,
evidence that existing handling is insufficient, and an account of legitimate
behavior the proposed restriction would prevent. Challenge unsupported requests
with code and contract evidence through the active review process; do not add
restrictions simply to satisfy a reviewer. When a defense is becoming complex,
consider whether removing it or simplifying the owning component would solve
the problem more cleanly.

## Escalate broader work

If addressing a finding requires a major refactor, architectural change, new
feature, or unrelated investigation, bring it to the user to decide how to
proceed before implementing that work. Judge scope by the responsibilities,
contracts, and behavior affected, not just the number of changed lines.

Explain the affected component and its purpose, the concrete issue and
evidence, why a local fix is insufficient, and the broader options and their
tradeoffs. Recommend an approach when the evidence supports one, and identify
the decision needed from the user. Record the finding and its current status
so it is not lost while awaiting that decision.

Continue independent local fixes while a broader issue awaits a decision.
Keep work that depends on that decision blocked. Do not silently defer a
correctness requirement and then report the change as complete, or implement
a brittle workaround just to avoid the scope discussion.

When reporting the response, connect each addressed finding to the actual fix
and verification. Keep unresolved findings, verification gaps, and requests
for a scope decision visible. Do not treat an unverified or disputed claim as
an established defect merely because an agent reported it.
