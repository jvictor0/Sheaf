---
name: xagent-subagents
description: Launch cross-provider review and worker subagents through the Conductor-managed xagent service, from any harness and any active repository.
---

# xagent Subagents

Use this skill when you need an external review opinion, a cross-provider
second pass, or a delegated worker through the Conductor-managed xagent
service. It applies to every harness — Claude Code, Codex, Cursor, and Pi.

## Reaching the service

xagent runs as one Conductor-managed service on `127.0.0.1:9005`. Everything
below talks to that service; nothing here starts a supervisor of its own.
Verify Conductor reports it healthy before launching a worker.

There are two client surfaces, and which one you have depends on your harness:

| Surface | Available when | Covers |
|---|---|---|
| HTTP MCP tools (`xagent_*`) | your harness has the `xagent` MCP server attached at `http://127.0.0.1:9005/mcp` | all supervised delegation |
| Packaged CLI (`xagent`) | always, once the plugin is installed | all supervised delegation |

Confirm the MCP tools before relying on them: list your available tools and
look for `xagent_start`. The packaged launcher lives at
`$HOME/.agents/plugins/plugins/xagent/scripts/xagent`.

**Pi ships without built-in MCP by design.** On Pi, use the packaged CLI for
delegation.

If the service is unhealthy or unreachable, or the MCP tools are missing on a
harness that should have them, surface broken agentic infrastructure and
inspect Conductor health. Do not launch an embedded, plugin-local, or
unmanaged replacement supervisor, and do not hand-roll your own MCP client.

Use the active repository as an absolute existing working directory for every
dispatch. Child harnesses run in that worktree. The executable runtime is
supplied by the installed Codex plugin rather than by the active repository.
Packaged xagent defaults persisted logs to the Sheaf central log root;
set `XAGENT_LOG_ROOT` only when intentionally validating or isolating logs
somewhere else.

## Generic Delegation

Use the sections below for reviews, implementation, fixes, and other delegation.

### Supervision Flow

The controller flow is:

```text
verify Conductor reports xagent healthy
→ xagent_start(cwd, prompt, harness/model/policy)
→ perform independent boss work
→ one xagent_await(run_id, after_sequence)
→ consume report.text, or handle compact attention
→ xagent_await again with the returned after_sequence only when continuing
```

Record the returned `run_id` and `after_sequence` cursor.

After `xagent_start`, do independent controller work until it is exhausted.
Then enter one long `xagent_await` with the latest cursor. Healthy provider
deltas, tools, raw events, status, and healthy watchdog verdicts never
complete an await and never enter the leader context.

On successful completion, consume the sanitized final assistant report from
`report.text` in the await result. Do not read the intermediate transcript,
tail logs, or summarize progress for the leader. If the service completes
without final text, treat that as `missing_final_report` and escalate.

When await returns compact attention instead of completion, read the attention
payload, act only at the controller layer, and call `xagent_await` again with
the returned cursor only when continuing supervision.

Use `xagent_message` to submit follow-up work or answer a worker on the same
run. Pass the complete instructions in `text`, including any review findings,
fix requirements, and context. Record the returned `sequence`, then await that
turn with `xagent_await`. Close the run with `xagent_close` when finished.

### Long Awaits Need a Patient HTTP Client

`deadline_seconds` is no longer advertised on the agent-facing `xagent_await`
schema. An await ends on news, not on a clock; while the supervisor vouches for
the run, progress notifications keep one await holding. A conforming client
must reset its request timeout on those notifications or configure a request
timeout at least as long as its intended await — the MCP SDK's 60-second
default does neither. If a long await dies because the client gave up while
the supervisor was still vouching, the run is still alive: recover it with
`xagent_list` and await again. Do not restart the worker.

### Recovering a Lost Run

`xagent_list` returns service-owned runs newest first. Use it when a start
response was lost, a client died mid-await, or you need to find an orphaned
run to close. Rows identify the run and its provider, phase, and timestamps.
This is a recovery tool, not a progress-polling tool.

### Reading a Failure

A failed run returns a `reason` and a payload that explains itself:
`provider_error` carries the provider's own message (a usage limit, a rejected
model, an API error), and `process_exit` carries the exit code plus whatever
the child wrote to stderr. Act on that text; do not guess at causes or probe
the provider by hand. The full provider transcript for any supervised run is
on disk at `<log root>/<run_id>/raw-provider.jsonl` — read it when diagnosing
a failure, never to watch a healthy run.

### No Routine Polling

While a worker is healthy and the controller has no independent work, do not
poll at a short fixed interval. Specifically, do not poll `write_stdin`,
`xagent list`, xagent logs, terminal status, or unchanged MCP inspect output
merely to observe progress.

Inspect supervision state only after attention, a supervision stop-vouching
wakeup, or an explicit user status request. On the supervised path the persisted
surfaces are lifecycle phase, attention events, and watchdog telemetry — not a
provider transcript; the service does not persist routine provider output to the
run logs. One long blocking await is the default wait mechanism.

### Quiet Service-Client Fallback

When plugin MCP discovery is unavailable but Conductor reports the xagent
service healthy and the packaged xagent CLI remains functional, use the quiet
`xagent supervise` service-client fallback instead of terminal polling:

```shell
XAGENT_PLUGIN_ROOT="${HOME}/.agents/plugins/plugins/xagent"
"${XAGENT_PLUGIN_ROOT}/scripts/xagent" supervise --harness claude_code --model sonnet "<prompt>"
"${XAGENT_PLUGIN_ROOT}/scripts/xagent" await <run_id> --after-sequence <n> --deadline-seconds 7000
```

Issue one application-level blocking await per wait cycle. The quiet client may
reissue shorter HTTP MCP request chunks under the hood (≤240 seconds) until that
deadline; treat those as an implementation detail, not a polling loop. Surface
the MCP discovery failure rather than hiding it. Use this fallback only when the
Conductor-managed service is healthy.

### Watchdog Boundary

Mechanical supervision is deterministic and never invokes Haiku: process
exit/spawn failure, turn completion/failure, transport failure, exposed
input/permission wait, cancellation/close, hard deadline, and silence.

Haiku is eligible only while a live worker is actively producing
tokens/messages/tools. It detects active semantic derailment or uncertainty.
Watchdog results are advisory only. Watchdog attention never messages,
interrupts, kills, restarts, edits for, or otherwise steers the worker.

### Review Routing

For review tasks, prefer a Claude-backed reviewer through `xagent_start`
with the `claude_code` harness:

- `opus`: strongest reviewer for subtle architecture, security, correctness,
  or release-risk reviews.
- `sonnet`: balanced default for ordinary code review.
- `haiku`: fast, small, inexpensive reviewer for narrow diffs, copy checks, or
  quick sanity passes.

Write the review prompt with the scope and output shape:

- Name the files, diff, PR, or task being reviewed.
- Ask for findings first, ordered by severity.
- Ask for concrete file/line references when available.
- Ask the reviewer to call out uncertainty instead of filling gaps.

Do not use stale dotted model names such as `claude-opus-4.8`; local Claude
Code accepts aliases such as `opus` or full names such as `claude-opus-4-8`.
If an unfamiliar model alias is needed, verify it with local Claude Code
before retrying. Do not silently downgrade to a weaker model after a model
rejection.

### Worker Routing

Use Cursor through xagent when a competent worker pass is useful. Treat
Composer 2.5 as a solid worker for straightforward implementation, cleanup,
alternate drafts, or exploratory passes.

For the trickiest implementation tasks, prefer a GPT or Codex-backed worker
agent instead of Composer.

### Legacy Terminal Protocol

`xagent run --subagent` remains available for harness compatibility, but the
default supervised path above is Conductor service MCP or the quiet
service-client fallback. Do not combine legacy terminal stdin polling with
service MCP for the same run. Legacy runs do not reliably exit on
`control.exit` and have leaked for days; prefer the supervised path, where the
service owns session close.

When xagent launches the Codex harness through either the supervised MCP path or the legacy terminal path, it passes Codex's explicit `--dangerously-bypass-approvals-and-sandbox` flag so the xagent-spawned Codex child does not stop for command approvals or inherit a restrictive sandbox. Use this only through xagent; do not copy that flag into unrelated workflows.

### Failure Handling

If the packaged launcher, MCP tools, Conductor service, Claude Code, Cursor
Agent, or a requested model cannot be used as instructed, surface broken
agentic infrastructure. Do not silently switch tools, rebuild xagent ad hoc from
a guessed Sheaf checkout, or work around broken agentic infrastructure.

Log inspection is reason-gated like other status checks, not a
polling loop.
