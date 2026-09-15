# Software Principles

Use this skill when making engineering decisions, reviewing code, implementing
changes, or deciding whether to continue after finding a suspicious condition.

Write good, clean code. Prefer simple, explicit designs over cleverness.

Maintain clean contracts between components. Make boundaries explicit, keep
responsibilities clear, and avoid accidental coupling.

Preserve legitimate behavior. Do not add restrictions, input rejection, or new
preconditions without a concrete reason grounded in a requirement, an
established invariant, or a credible failure mode. An unfamiliar use case is
not an invalid one. Identify what a proposed restriction protects and which
otherwise valid uses it would exclude.

Validate at the boundary that owns the rule. Client-side pre-validation should
provide a concrete benefit, such as useful immediate feedback or avoiding an
expensive doomed operation. Do not duplicate authoritative validation without
a reason, impose stricter client rules than the underlying interface requires,
or try to predict every downstream failure. Let the responsible component
handle cases it already supports.

Defensive code must earn its complexity. Be able to explain the failure it
prevents, why that failure is plausible here, and why existing contracts or
error handling are insufficient. As guards, fallbacks, and special cases grow,
reconsider whether the defense is needed at all. Prefer removing an unnecessary
defense or simplifying the responsible contract over adding another layer.
Retain protections that serve a concrete requirement or failure mode; complexity
alone is a reason to reassess them, not proof that they are unnecessary.

Prefer assertions for states ruled out by established internal invariants.
Do not declare legitimate inputs or supported states impossible to simplify
the implementation. It is often better to fail loudly on an invariant violation
than to hide a bug through silent recovery.

Keep tests green. Do not change tests to work around bugs. If a test is wrong,
say so clearly and explain why before changing it.

If a harness, skill, CLI, installer, or other agentic infrastructure is broken,
do a minimal investigation before stopping: inspect the relevant error and
immediate health or configuration evidence to identify the likely cause.
Then stop the affected task and escalate to the user with what failed, what
you checked, and what is known or still uncertain. Do not turn unrelated work
into an infrastructure repair project or work around the broken infrastructure.

This stop-and-escalate rule does not apply when the task itself is to develop,
debug, or repair agent infrastructure. In that case, investigate and fix it
within the authorized scope.

If you see something, say something. Surface suspicious behavior, weak
assumptions, inconsistent design, missing tests, or risks likely to matter.
