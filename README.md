# sheaf

Sheaf is a local-first chat server with a queue-backed worker, websocket streaming, and a turn-ledger database.

## Current Architecture

- FastAPI API + websocket transport
- SQLite ledger for threads, turns, queue, request logs, events, and errors
- Single background worker loop (no worker concurrency)
- Exponential retry with no max attempts for non-fatal failures
- Fatal queue failures moved to `queue_errors`
- Client reconnect model: ledger is source of truth

## Core APIs

- `GET /health`
- `GET /models`
- `POST /models/updateLocalModelList`
- `POST /threads`
- `GET /threads`
- `POST /threads/{thread_id}/archive`
- `POST /threads/{thread_id}/unarchive`
- `POST /threads/{thread_id}/enter-chat`
- `WS /ws/chat/{session_id}`

## Websocket Protocol

All frames include:

- `protocol_version`
- `type`
- `session_id`
- `server_time`

Client -> server:

- `submit_message`

Server -> client:

- `handshake_snapshot_begin`
- `committed_turn`
- `message_durable_ack`
- `assistant_token`
- `turn_event`
- `context_budget`
- `heartbeat`
- `turn_finalized`
- `execution_conflict`
- `error`

## Worker and Queue Semantics

- Worker claims one runnable queue row at a time (`locked_by/locked_at`)
- Non-fatal errors:
  - `attempts += 1`
  - exponential backoff
  - capped at 10 seconds
  - retried indefinitely
- Fatal errors:
  - moved to `queue_errors`
  - removed from live queue

## Data Layout

- `data/server.sqlite3` primary server DB
- `data/user_dbs/` user SQLite databases
- `data/system_prompts/` prompt files
- `data_archive/` archived legacy data snapshots

## Agent Tool Calls

Tools are agent-internal and not exposed via public server endpoints.

Current agent tools:

- `write_note`
- `list_notes`
- `read_note`
- `list_sqlite_databases`
- `create_sqlite_database`
- `run_sql`

When the model emits function/tool calls, the dispatcher executes these tools
and feeds tool results back into the model loop before final assistant output.

## Local Setup

```bash
python3 -m venv .venv
. .venv/bin/activate
.venv/bin/python -m pip install -e .[dev]
```

Run server:

```bash
.venv/bin/python run_server.py
```

Run tests:

```bash
PYTHONPATH=src .venv/bin/python -m pytest -q
```
