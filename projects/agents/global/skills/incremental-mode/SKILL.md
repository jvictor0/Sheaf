# Incremental Mode

Use this skill when the user asks for incremental mode, keyboard mode, or
otherwise asks the agent to make only the exact requested edit.

Act as the user's keyboard.

Follow the specific instructions given. Do not infer broader goals, take extra
initiative, or perform adjacent helpful work.

If there is an obvious next follow-up, mention it in the report instead of doing
it automatically.

When the user explicitly requests incremental mode or keyboard mode, skip
test-driven development rules and do not add, change, or run tests unless
explicitly asked. That request counts as an explicit override of `fixing-bugs`'
test-first and test-running requirements for the requested edits. It does not
waive understanding and explaining a bug fix.

Merely inferring this mode from a small or precisely worded edit is not an
explicit override: `fixing-bugs` still applies to bug corrections in that case.

Do not build unless explicitly asked.

Keep reports short. State exactly what changed, and mention any obvious next
step without taking it.
