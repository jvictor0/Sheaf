# xagent Event-Driven Supervision — Operations and Rollback

## Service lifecycle

The `xagent` service is a long-lived, Conductor-managed process that owns worker runs over streamable HTTP MCP. It binds only to loopback.

### Start

```bash
make xagent-service-run
```

This runs `node dist/src/service_main.js` from `projects/xagent`. The service reads `config/services.json`, binds to `127.0.0.1:9005`, and serves MCP at `http://127.0.0.1:9005/mcp`.

The listener binds BEFORE startup reconciliation runs, so a duplicate `make xagent-service-run` while Conductor's copy is already up exits on `EADDRINUSE` without reconciling, signalling, or abandoning any run owned by the running instance. Reconciliation then runs against the log root only this instance can reach, and its degradation outcome is wired into `/health`.

### Health check

```bash
curl -s http://127.0.0.1:9005/health
```

Returns `200` with `{ "healthy": true, "uptime": <seconds> }` while the service is accepting connections.

### Stop (orderly shutdown)

```bash
curl -s -XPOST http://127.0.0.1:9005/exit
```

Returns `{ "exiting": true }`, stops accepting new connections, closes every owned session/process group, and exits `0`. Conductor stops the service primarily via `POST /exit`; a direct `SIGTERM` or `SIGINT` to the process is also trapped and drives the same orderly shutdown (close owned provider sessions and process groups, then exit `0`), so Conductor's stop fallback (which sends `SIGTERM` when `POST /exit` fails or is unresponsive) and a human Ctrl-C no longer orphan detached provider process groups. A repeated signal forces a non-zero exit so a wedged orderly shutdown cannot block escalation to `SIGKILL`.

## Endpoint and tools

| Path | Method | Purpose |
| --- | --- | --- |
| `/health` | `GET` | Liveness and uptime |
| `/exit` | `POST` | Orderly shutdown |
| `/mcp` | `POST` | Streamable HTTP MCP |

The control plane is unauthenticated: `/mcp` has no token, no peer-credential check, and no unix-socket option. The bind host and DNS-rebinding guards mitigate the LAN and browser vectors, but loopback is not the same as same-user trust — any local account on the host can POST to `127.0.0.1:9005/mcp` and start a fully unsandboxed agent (`xagent_start_non_sdd` accepts any absolute existing `cwd`, and the Codex adapter passes `--dangerously-bypass-approvals-and-sandbox`) running as the service's user. This is an acceptable posture on a single-user dev machine and is the explicit trust assumption of the current design; a shared-token or unix-socket transport is the cheap hardening if that assumption ever breaks (review I3).

The nine MCP tools exposed by the service:

1. `xagent_start_non_sdd` — create a run from an absolute existing `cwd`, a `prompt`, a `harness`, and an optional `mode`/`policy`.
2. `xagent_message` — send a new user message to a running worker.
3. `xagent_await` — block until the next deliverable event after `after_sequence`.
4. `xagent_inspect` — return the compact phase/sequence snapshot for a run without blocking.
5. `xagent_list` — list service-owned runs newest first (recovery, not progress polling).
6. `xagent_interrupt` — request a cooperative interrupt of the active turn.
7. `xagent_close` — close the run and release owned process resources.
8. `xagent_sdd_start` — render a Superpowers SDD role prompt, reserve the ledger row, and start the owned provider session.
9. `xagent_sdd_followup` — submit a fix or re-review follow-up on an existing SDD session.

Dedicated `xagent_sdd_await` and `xagent_sdd_close` tools are deleted; controllers await and close SDD runs with the generic `xagent_await` / `xagent_close` tools keyed by `run_id`.

## Superpowers SDD facade

Superpowers subagent-driven development (SDD) uses two facade tools on top of the generic supervision stack. Controllers MUST use these tools for implementer, reviewer, fixer, and re-reviewer turns instead of assembling raw prompts. Use `xagent_sdd_followup` (or a fresh `fixer` / `re-reviewer` start) for work dispatch; `xagent_message` is legal on SDD runs for unstructured answers only.

### Tool contracts

#### `xagent_sdd_start`

Input (discriminated by `role`):

- Common fields: absolute existing `cwd`, absolute `plan`, `model`, `harness`, `effort`, optional `policy`, optional `note`.
- `implementer`: `task`, `name`, absolute `brief`, absolute `report_out` (path the agent writes), optional `context`.
- `reviewer` with `task` (task-scoped): absolute `brief`, absolute `implementer_report` (implementer's existing report the reviewer reads), `base`, `head`, optional absolute `constraints`, and `diff` unless the plan workspace already holds the derivable `review-<base>..<head>.diff`.
- `reviewer` without `task` (whole-branch): absolute `brief`, `description`, `base`, `head`. Whole-branch reviewer sessions are single-turn (`start` → await → `close`) with no follow-up.
- `fixer`: `task`, absolute `brief`, absolute `findings`, `findings_text`, non-empty `tests` array, absolute `report_out` (path the agent writes), optional `round` (default 1).
- `re-reviewer`: `task`, absolute `brief`, absolute `findings`, absolute `fixer_report` (fixer's existing report the re-reviewer reads), `base`, `head`, optional `round` (default 1), and `diff` unless the derivable review-package file exists.

Success output:

```json
{
  "run_id": "<stable xagent run id>",
  "sequence": <pre-turn cursor>,
  "brief_path": "<absolute brief path>",
  "report_out_path": "<absolute resolved report path when the role writes one>",
  "prompt_path": "<absolute rendered prompt path when the role uses one>",
  "renderer_path": "<absolute dispatch-prompt path when the role uses one>"
}
```

`fixer` omits `prompt_path` and `renderer_path`. Roles that do not write a report omit `report_out_path`. The result never includes copied brief text, findings text, or rendered prompt bodies.

#### `xagent_sdd_followup`

Input (discriminated by `kind`):

- `fix` (implementer or fixer sessions): `run_id`, `round`, absolute `findings`, `findings_text`, non-empty `tests` array, absolute `report_out`.
- `re-review` (reviewer or re-reviewer sessions): `run_id`, `round`, absolute `findings`, absolute `fixer_report`, `base`, `head`, and `diff` unless the derivable review-package file exists.

Success output:

```json
{
  "run_id": "<same run id>",
  "sequence": <pre-turn cursor>
}
```

Await and close with the generic tools: `xagent_await({ "run_id", "after_sequence" })` and `xagent_close({ "run_id" })`. On successful completion, consume the sanitized final assistant report from `report.text` in the await result.

### Prompt rendering prerequisites

SDD start and re-review follow-ups render prompts through the trusted Python executable at `<service repoRoot>/projects/agents/utils/dispatch-prompt`. The renderer subprocess uses the canonicalized `cwd` from the start request as its working directory; the controller's own cwd is not consulted.

Rendering also requires an installed Superpowers template tree. By default `dispatch-prompt` reads templates from the installed Superpowers plugin cache; operators may pin templates with `SUPERPOWERS_TEMPLATES_ROOT` or the renderer's `--templates-root` flag (surfaced through the SDD prompt layer for tests). Missing Python 3, a missing trusted renderer, or missing templates fail before any ledger row or provider process is created, with structured MCP codes `sdd_python_missing`, `sdd_renderer_missing`, `sdd_renderer_failed`, `sdd_renderer_bad_input`, `sdd_renderer_output_invalid`, or `sdd_prompt_unreadable` rather than a generic `tool_failed`.

### SDD ledger database

- Path: `<service logRoot>/sdd.sqlite` where `logRoot` is `resolveXagentLogRoot(repoRoot)` (typically `<sheafRoot>/data/xagent`).
- Schema version: `user_version = 2` with a single immutable `sdd_agents` table. The ledger column for the run identity remains `agent_id`; the public MCP tools name that same value `run_id`.
- Permissions: the log root directory and `sdd.sqlite` (+ WAL sidecars) are owner-only (`0700` directory, `0600` files).
- Rows are written once at dispatch and never updated or deleted. Mutable state — phase, delivered reports, submitted prompts, failures — lives in the supervisor-owned run directory at `<log_root>/<run_id>/`, not in SQLite.
- A v1 ledger file is refused outright; there is no migration path from `sdd_sessions` / `sdd_turns`.

### Generic-tool behavior on SDD runs

- `xagent_message` is legal on SDD-owned `run_id`s for unstructured answers; work dispatch (fix / re-review) still goes through `xagent_sdd_followup` or a fresh start.
- Controllers await and close SDD runs with `xagent_await` and `xagent_close` keyed by `run_id`.
- Consume the sanitized final assistant report from `report.text` in the await result (`turn.completed`); do not treat the mutable Superpowers report file on disk as the delivered report.

## Final envelope

A successful `xagent_await` returns the complete sanitized final assistant report inline:

```json
{
  "run_id": "<id>",
  "event": "turn.completed",
  "sequence": <int>,
  "report": { "text": "<complete final assistant message>" }
}
```

Routine deltas, tools, raw events, status, and healthy watchdog verdicts never complete an await. Missing final text yields `missing_final_report`.

## Deterministic vs Haiku boundary

Mechanical states are classified deterministically and never invoke Haiku:

- process exit / spawn failure
- turn completion / failure
- transport failure
- exposed input / permission wait
- cancellation / close
- hard deadline
- silence timeout

Haiku is eligible only while a live worker continues emitting provider JSON. It runs fresh with `--safe-mode`, `--tools ""`, `--strict-mcp-config`, an empty MCP config, `--no-session-persistence`, structured JSON output, bounded input/output, and no repository working directory. The system prompt describes the input as bounded provider JSON and treats repeated tools, retries, failures, empty deltas, and unfamiliar transport records as ambiguous rather than deterministic derailment evidence.

## Watchdog defaults

| Knob | Default |
| --- | --- |
| Active-work cadence | 10 / 20 / 40 minutes |
| Minimum interval between checks | 5 minutes |
| Healthy confidence floor | `0.8` |
| String field bound | 16 KiB UTF-8 with marker `[xagent: truncated]` |
| Input bound | 64 KiB UTF-8 |
| Output bound | 2 KiB (applied to the normalized structured verdict with evidence truncated to a per-item UTF-8 byte bound, not the Claude Code JSON envelope) |
| Maximum calls per run | 8 |

Each Haiku request is exactly six fields: `original_prompt`, `harness`, `recent_provider_json`, `elapsed_ms`, `truncated`, and `input_bytes`. The evidence window retains newest complete `raw.provider` payloads that fit; an individually oversized record is omitted as a whole rather than summarized. Provider JSON and the task prompt are not redacted before the isolated Haiku call; classifier input is never persisted in supervision telemetry. Provider adapter normalization defects outside the watchdog evidence path are not repaired by this evidence model.

`raw.provider` records advance periodic watchdog eligibility without changing the health monitor's semantic-versus-transport timestamps or deterministic exposed-wait deduplication.

Watchdog results are advisory only. `derailed`, `uncertain`, invalid, failed, over-budget, and low-confidence output emit one sequenced attention event but never message, interrupt, kill, restart, edit for, or otherwise steer the worker.

### False-alert posture

The "false-alert posture" documented here describes the **supervisor's response boundary to fixture verdicts**, not Haiku's classifier accuracy. The fixtures in `tests/fixtures/watchdog/` carry pre-set `scripted_verdict` fields; the tests measure how the supervisor reacts to a given verdict (silent for healthy, one advisory attention for `uncertain`/`derailed`), not how often Haiku is wrong in production. Classifier accuracy against a live `claude` binary has not been measured in this branch — see "Live watchdog smoke" below for the optional gate that exercises the spawn path.

The watchdog fixtures document the supervisor response boundary for each scenario:

- **Healthy exploration**: a high-confidence healthy verdict (`confidence >= 0.8`) stays controller-silent. No attention is emitted. This is the primary false-alert guard: routine diverse work never wakes the controller. Claude Code production fixtures also assert that raw `input_json_delta` arguments and `tool_result` content remain visible in the watchdog request at the periodic checkpoint.
- **Repeated tool / failure loops**: repetition never triggers an early check. Haiku evaluates repeated tools or failures only at a periodic checkpoint; the repeated-tool and repeated-failure fixtures are regressions for zero classifier calls before ten minutes.
- **Task contradiction / insufficient evidence**: only the periodic Haiku check detects these. An `uncertain` verdict (e.g., from a single short delta) emits one advisory attention so the controller can decide — it is a true uncertain verdict, not a false alert.
- **Silence / crash**: zero classifier calls. These are deterministic and advisory; the controller decides whether to wait, interrupt, or close.

In all cases the worker is never auto-interrupted, auto-killed, or auto-restarted by the watchdog.

### Live watchdog smoke

The watchdog argv has never been executed against the real `claude` binary by default `npm test` — every `claude_watchdog.test.ts` assertion injects a fake `WatchdogSpawn`. The argv flags are validated against `claude --help` by hand, but no in-tree test or packaged smoke actually invokes `claude`.

Execution evidence for the gate below is recorded in `.superpowers/sdd/2026-07-25-xagent-event-driven-supervision/watchdog-live-smoke-evidence.md` (command, date, `claude` version, captured verdict, and pass/fail summary). That artifact is the human-readable record that the documented pre-production gate was exercised; re-run the smoke and append a new entry whenever the argv or the installed `claude` CLI changes.

To close that gap without flaking default CI, an **optional** live smoke is provided:

- `tests/claude_watchdog_live.test.ts` invokes the real `claude` binary through `ClaudeWatchdogClassifier.classify()` (the production spawn + envelope-extraction + normalization path, using the production `DEFAULT_STDOUT_LIMIT_BYTES` stdout cap) only when `XAGENT_RUN_LIVE_WATCHDOG_SMOKE=1` is set **and** `claude` is on `PATH`. Otherwise it skips with a clear message.
- It is **not** run by `npm test`. Run it explicitly:

```bash
XAGENT_RUN_LIVE_WATCHDOG_SMOKE=1 node --test dist/tests/claude_watchdog_live.test.js
```

- It does not assert classifier accuracy (Haiku may legitimately return `uncertain` for the tiny synthetic input). It asserts the production `classify()` path returns a real verdict shape — `verdict` is one of `healthy`/`derailed`/`uncertain`, `reason_code` is non-empty, `evidence` is an array, and the normalized verdict fits within the 2 KiB verdict byte bound — so envelope extraction, usage/cost extraction, and the `--tools ""` (Commander variadic → `[""]`) semantics are exercised against the installed binary. A network/API/budget/timeout failure is tolerated: the classifier's `*_failed`/`*_timeout`/`*_exceeded`/`invalid_classifier_output` uncertain verdicts are a pass, since the goal is to prove the spawn/parse/classify plumbing rather than Haiku's correctness.

## Wake comparison

A 90-minute healthy run with sustained semantic progress (one delta per minute under the default 5-minute silence timeout) compares controller-visible wake counts as follows. The 90-minute rows in this table are **run-manager-layer** measurements: `supervision_cost.test.ts` and `mcp_await.test.ts` drive `XagentRunManager.awaitRun(...)` directly with a fake clock, so they prove the supervisor wakes the leader exactly once for the terminal `turn.completed` event. They do **not** exercise the HTTP transport that a real controller sits behind.

| Wait mode | Wake count | Leader-visible progress in completion envelope |
| --- | --- | --- |
| 30-second terminal polling (`xagent_inspect` every 30 s) | 180 (analytic) | none — inspect snapshots are not completion envelopes |
| Quiet CLI fallback (`start` then one blocking `await`) | 1 (measured at run-manager layer) | final report only — no deltas, tools, or progress fields |
| MCP await (`xagent_await`) | 1 (measured at run-manager layer) | final report only — envelope shape asserts absence of deltas/tools/progress |

The MCP await and quiet CLI paths wake exactly once for the terminal `turn.completed` event. Routine deltas, tools, raw events, status, and healthy watchdog verdicts never complete an await. Parent-side token or byte totals are not measured here; the tests instead assert that completion envelopes omit non-terminal progress fields.

### Transport provenance — what is and is not verified

The long-await claim is load-bearing for the whole design, so the test boundary is documented here explicitly:

- **Quiet CLI fallback (`xagent start`, then `xagent await`)** — `start` returns the durable run id immediately. The service client holds one `xagent_await` HTTP request and resets its request timeout on 30-second MCP progress notifications. `tests/service_client.test.ts` exercises the real HTTP transport and asserts that a long healthy wait issues exactly one await tool call. Advisory watchdog attention is written to stderr and the CLI keeps waiting; deterministic attention and terminal results return on stdout.
- **Primary Codex MCP path** — the packaged `.mcp.json` connects Codex directly to `http://127.0.0.1:9005/mcp` with `tool_timeout_sec: 7200` and **no chunking**. Whether Codex's HTTP client holds a single 90-minute idle response body is **not verified by this branch's tests**. The 90-minute rows above are fake-clock run-manager tests, not transport tests against the Codex client. If Codex's HTTP stack has an undici-like idle ceiling, the plugin path will need the same chunking the quiet client already has; that work is tracked as a follow-up rather than asserted here.
- **90-minute healthy-run tests** — `mcp_await.test.ts` ("ninety-minute healthy run completes without an intermediate deadline wake") and `supervision_cost.test.ts` ("90-minute healthy run: MCP await wakes once; quiet client measured; polling analytic") both advance a `FakeClock` and call `runManager.awaitRun(...)` directly. They prove the supervisor does not wake the leader for routine progress; they do **not** prove a real HTTP body survives 90 minutes.

## Timeouts

| Setting | Value |
| --- | --- |
| `xagent_await` default deadline | 7000 seconds |
| `xagent_await` maximum deadline | 7000 seconds |
| Quiet-client MCP request idle timeout | 60 seconds, reset by 30-second MCP progress notifications. No fixed-duration polling or request chunking. |
| Plugin MCP `tool_timeout_sec` | 7200 seconds (no chunking on the primary Codex path — see "Transport provenance" above; the 90-minute single-POST assumption is unverified at the transport layer) |
| Service request timeout | 7,200,000 ms |
| Service headers timeout | 7,270,000 ms |

## Recovery behavior

- An MCP/client disconnect or cancelled await releases only request-local resources; the Conductor-managed service continues owning the worker.
- A controller can reconnect by `run_id` and call `xagent_inspect` / `xagent_await` to reattach.
- An orderly service exit closes every owned session and process group.
- On restart, reconciliation marks unattachable active runs `abandoned` and kills a process only when PID plus process-start identity matches persisted ownership.

### Service-crash boundary

Provider reattachment across an xagent service crash is **out of scope**. If the service process itself crashes, owned worker processes are orphaned; the service does not reattach to them on restart. Restart reconciliation marks unattachable active runs `abandoned` and only kills a process when PID plus process-start identity matches persisted ownership. A controller that needs to survive a service crash should treat the run as terminated and start a new run.

## Central logs

Logs are written under the configured log root (resolved from `getDefaultLogRoot(repoRoot)`). Each run records its lifecycle phase transitions, attention events, and watchdog evidence for post-hoc review.

## Rollback to legacy CLI

If event-driven supervision must be bypassed, invoke the legacy in-process runner directly:

```bash
xagent run --subagent
```

or

```bash
xagent run --full
```

These keep the existing parsing, persistent stdin protocol, output filtering, and logs without starting or contacting the service.
