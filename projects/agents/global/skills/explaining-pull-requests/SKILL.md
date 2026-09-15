# Explaining Pull Requests

Use this skill when writing a PR title or description, explaining an existing
PR, or revising its explanation as the implementation changes. Follow
`communicating-with-user` for general communication guidance. This skill adds
the context a reader needs to understand the purpose and effect of a PR.

Write for someone who understands the project but has not followed the task's
conversation or memorized the affected subsystem. The explanation should let
them understand why the change exists before they study the diff.

## Establish the context

Read the actual diff and the surrounding code and requirements needed to
understand it. Use the PR's final scope, including related changes across
components. Do not derive the explanation solely from commit messages, file
names, or the author's completion report.

Explain what the affected part of the system does, who or what uses it, and
the current limitation or failure motivating the change. Introduce component
names with their responsibilities. When the change crosses boundaries, explain
the relevant flow between callers, services, storage, tools, or user interfaces
so the reader can place each edit in the system.

## Explain the change and its consequences

Connect the implementation to its purpose. Cover the applicable points:

- **Why this PR exists:** the user need, requirement, bug, or technical
  constraint it addresses, and why the previous behavior was insufficient.
- **Which components change:** identify their responsibilities and explain
  what changes in each. Group related edits by behavior or responsibility;
  a list of modified files does not explain the change.
- **How the change works:** describe the important mechanism and why it
  addresses the problem. Include contracts, data flow, or design tradeoffs
  where they help the reader assess the implementation.
- **Capabilities enabled:** explain what users or other components can now
  do, with a concrete example where useful. Distinguish a capability delivered
  by this PR from groundwork that still needs subsequent implementation.
- **Bugs fixed:** describe the triggering conditions, the incorrect result,
  its cause, and the corrected behavior. Make the before-and-after difference
  explicit rather than saying only that error handling or validation improved.
- **Evidence and limits:** describe relevant verification and what it proves.
  Explain remaining limitations, compatibility effects, migration or rollout
  requirements when they materially affect use or review of the change.

Avoid filling every category when it does not apply. An internal refactor may
preserve behavior while making a specific maintenance task easier; explain
that benefit without inventing a new user-facing feature. Ground motivation
in available evidence and distinguish inferred rationale from documented intent.

## Present the final implementation

Lead the title and description with the concrete problem or resulting behavior.
Develop the explanation in the order that makes the change easiest to understand:
context and motivation, mechanism and affected components, then results and
verification. Use the repository's PR template when present, without letting its
headings substitute for an explanation.

When the scope changes, rewrite the title and description around the final
implementation. Omit conversation history and abandoned approaches unless
they explain a relevant tradeoff. Include links to requirements and useful
code locations as supporting detail, not as a replacement for the context.

Scale detail to complexity. A small fix may need only a few connected sentences
and verification; a change spanning components needs enough surrounding context
to explain their interaction. The reader should be able to answer what changed,
why, where it belongs, and what difference it makes without reconstructing those
answers from the diff.
