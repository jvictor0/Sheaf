# Fixing Bugs

Use this skill when correcting a bug, including bugs found by tests or code
review. Follow this order: reproduce with a failing test, understand and explain
the fix, then change the implementation and verify it.

**Only an explicit instruction from the user can override this sequence.**
Urgency, apparent simplicity, difficulty writing a test, a reviewer's proposed
patch, or an agent's confidence is not an override. Apply any user override only
to the steps and scope it actually covers. Do not request an exception merely
to save effort.

## Reproduce before fixing

Use `writing-good-tests` for test design and assertion quality.

Identify the triggering conditions, observed behavior, and expected behavior.
Find an existing test or write a regression test that exercises the actual
bug and asserts the expected behavior. Run it against the unfixed implementation
and observe it fail for the reported defect. Record the command and relevant
failure output. A failure caused by broken setup or an unrelated error does
not establish reproduction; neither does a test that already passes.

Prefer a regression test in the project's normal test suite. If there is no
suitable framework, a saved, repeatable executable test with explicit
assertions is acceptable. An informal manual check is not a substitute.
For timing-dependent bugs, control the relevant ordering or conditions where
possible so the test demonstrates the failure rather than relying on luck.

Reading code, gathering diagnostics, and developing the reproducing test are
part of investigation. Do not change the implementation to fix the bug before
the reproduction and explanation steps are complete. If reproduction remains
unavailable, explain what is missing and continue investigation; do not proceed
with a speculative fix without the user's explicit override.

## Understand and explain before editing

Trace the failure to its cause in the responsible component. Understand the
relevant contracts and data or control flow, and identify where the intended
behavior breaks down. A change that happens to make the test pass is not enough.

Before editing implementation code, explain to the user—or to the coordinating
lead when working as a subagent:

- Which component is responsible and what it is supposed to do.
- How the triggering conditions cause the observed failure, supported by the
  reproduction and code evidence.
- What you will change and why that change removes the cause while preserving
  the intended surrounding behavior.
- How the reproducing test will demonstrate the correction.

This is an explanation, not an additional approval checkpoint. Once the test
has reproduced the bug and the causal fix is understood and explained, proceed
within the authorized scope.

## Make the fix fit the design

Make the code read as though it was designed to handle this case all along.
Correct the responsible contract, state transition, or algorithm at the right
boundary. Use existing abstractions and keep the behavior explicit. Avoid
special cases tied to the reported example, duplicated logic, silent error
suppression, and speculative retries or fallbacks that merely hide the defect.

A clean fix is not necessarily the smallest textual diff. A focused local
restructuring may express the correct behavior more clearly than an extra
conditional. Keep it proportional to the bug; a major refactor or unrelated
cleanup requires a scope discussion with the user before implementation.

Rerun the same reproducing test after the fix and confirm it passes for the
expected reason. Keep the regression test, and run the additional checks needed
for the affected behavior and contracts. Do not weaken its assertions to obtain
a passing result. If the test expectation was wrong, explain the correction
and establish a valid failing reproduction before continuing.

If the fix fails, return to the evidence, revise the causal explanation, and
explain the revised fix before attempting another implementation change. Do
not stack speculative patches. Report the actual cause, correction, test
results before and after, and any remaining verification limits.
