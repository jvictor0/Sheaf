# Operational Decisions (First Pass)

## Scope

Captures explicit decisions for this personal single-user deployment so implementation does not over-engineer around multi-tenant assumptions.

## Security And Access Model

- Deployment target is personal use on a VPN.
- Single client device: phone.
- No server-side authentication layer is required for this first pass.

## Message Delivery And Idempotency

- No explicit idempotency key is required.
- Client does not immediately retry lost sends.
- If duplicate logical sends happen later, conflict handling is via `in_response_to_turn_id` CAS checks against thread tail.
- If same message is submitted twice against same prior turn, one execution should eventually fail CAS.

## Protocol Compatibility

- Protocol versioning is required.
- Handshake and message envelopes must include `protocol_version`.
- Server should reject incompatible protocol versions with a clear error.
- Interop non-requirement

## Queue Locking And Process Model

- Single server process model.
- No lease heartbeat / timeout semantics required in first pass.
- A worker lock is held until process exits.
- On startup, process clears `message_queue.locked_by` and `message_queue.locked_at` before accepting work.

## Schema Bootstrap And Upgrades

- Maintain a schema scripts directory with:
  - one bootstrap script for base schema
  - incremental upgrade scripts for later changes
- Migration execution modes:
  - bootstrap + all upgrades (fresh install)
  - next unapplied upgrades only (existing install)
- Migration scripts must be idempotent.
- Backups must be taken as part of migration workflow.

## Backup Policy (Current)

- During active code/schema iteration, favor very frequent backups.
- Current preference: backup after each meaningful diff/change step.
- This policy is intentionally conservative because highest break risk is during rewrite iteration.

## SQLite Runtime Defaults

- Use write-ahead logging: `PRAGMA journal_mode = WAL;`.
- Use `PRAGMA synchronous = NORMAL;` for first pass.
- Set `PRAGMA busy_timeout = 5000;` to reduce transient lock failures.
- Keep checkpoint policy simple initially (SQLite auto-checkpoint defaults are acceptable for first pass).

## Deferred / Not In Scope Yet

- Tool sandbox hardening and extra path security controls.
- Operational limits (queue depth, payload caps, trace caps).
- Full latency SLO policy and advanced metrics.
- Multi-process coordination semantics.

## Follow-Up Topics To Specify

- Migration script naming/version convention and tracking table details.
- Concrete backup automation command flow.
