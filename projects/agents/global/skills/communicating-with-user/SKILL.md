# Communicating with the User

Use this skill for progress updates, technical explanations, and final reports.
Be detailed and specific enough for the user to understand what happened, why
it matters, and what remains. Do not assume they remember the implementation
details or can infer a component's purpose from its name.

## Give facts their context

Lead with the outcome or the point that needs attention. Name the affected
component and explain its responsibility when introducing it. Connect file,
symbol, or service names to the behavior they control. Include surrounding
data flow, callers, dependencies, or constraints when they explain the issue
or the choice of solution.

Describe concrete behavior: what triggered the issue, what happened before,
what changes afterward, and why. Prefer a specific example when it clarifies
the distinction. Explain the causal connection between a change and the result;
“fixed validation” or “updated the handler” alone does not tell the user enough.

Separate observed facts from hypotheses, unresolved questions, and proposed
actions. Explain uncertainty and its practical effect. Use precise references
and links to relevant files or artifacts so the user can inspect the details.

## Progress updates

State what you have learned or changed, why it matters, and what the next step
will resolve. Describe blockers in terms of the affected behavior and the
missing information or decision. Explain unfamiliar tools or helpers by their
purpose rather than reporting only an error name. Include meaningful changes
in direction without narrating every command or repeating unchanged status.

## Final reports

Make the final response self-contained; the user should not need to read prior
progress updates to understand it. For substantive work, explain:

- What changed, in which components, and what those components do.
- The original problem or requirement, the relevant surrounding context, and
  why the chosen change addresses it.
- What was verified, what the results establish, and any checks not completed.
- Remaining issues, deferred work, or decisions needed, with their impact.
- Where to inspect the implementation and supporting artifacts, and the
  actual delivery state when relevant: written, committed, installed, or deployed.

Scale detail to the work and the user's question. Use connected prose for
explanations and lists or tables for genuinely parallel information. Include
the evidence and context needed to assess the result; avoid padding, generic
assurances, raw command dumps, and unexplained implementation shorthand.
