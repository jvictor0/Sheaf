# Rewrite Roadmap

This directory contains first-pass documents for the server rewrite.

Current scope:
- Fresh SQLite schema (no data migration)
- Tables split by component
- Server transport and execution flow draft

Documents:
- `01_threads_and_turns.md`
- `02_message_queue.md`
- `03_turn_events.md`
- `04_models_and_requests.md`
- `05_chat_transport_and_sync.md`
- `06_turn_execution_and_commit.md`
- `07_failure_recovery_and_reconnect.md`
- `08_dowork_runner_contract.md`
- `09_tooling_surface.md`
- `10_rest_api_catalog.md`
- `11_operational_decisions.md`
- `impl_notes.md`

Assumptions currently encoded in schema:
- UUID values are stored as `TEXT` in SQLite.
- Queue items can target a specific turn or be null.

These are working drafts for implementation planning.

## Configuration policy

- Server configuration is loaded from `config.json` at startup.
- Secrets are loaded from `secrets.json` at startup.
- Environment variables are not used for configuration or secrets.

## Data policy

- Data root includes `data/user_dbs/` for user SQLite databases.
- Data root includes `data/system_prompts/` for selectable system prompt files.
- Agent must not write directly to the system database; agent DB writes are limited to user databases.
