# sheaf

`sheaf` is a Python server-hosted conversational agent using they/them pronouns.

## Current architecture

- FastAPI web server
- Chat state persisted on disk per chat
- Checkpoint snapshots stored under each chat directory
- LLM dispatch isolated behind an abstract interface
- LLM path now defaults to a LangChain chat chain (OpenAI model backend)
- Runtime conversation state/checkpoints are now managed by LangGraph (SQLite checkpointer)
- Runtime pre-assistant compaction uses model-specific limits (`ModelProperties`)
- Tool calling is enabled for controlled note writes under `data/notes`

## API

- `POST /chats` allocates a new `chat_id` (chat directory is created lazily on first message)
- `GET /chats` lists chats from disk
- `GET /chats/{chat_id}/metadata` returns per-chat metadata and message count
- `GET /chats/{chat_id}/messages?start=<int>&end=<int>` returns a message slice (start inclusive, end exclusive)
- `POST /chats/{chat_id}/messages` appends user message, calls LLM, stores a new checkpoint, and returns:
  - `chat_id`
  - `response`
  - `checkpoint_id`
- `POST /admin/reboot` requests a supervisor reboot of both API + Chainlit (dev use)

## Secrets (required)

Create a local secrets file from the example:

```bash
cp .secrets.example.json .secrets.json
```

Then set your OpenAI key in `.secrets.json`:

```json
{
  "openai": {
    "api_key": "YOUR_OPENAI_API_KEY"
  }
}
```

Notes:
- `.secrets.json` is gitignored and must never be committed.
- Environment variable `OPENAI_API_KEY` takes precedence over file secrets.

## Setup

```bash
python3 -m venv .venv
. .venv/bin/activate
pip install fastapi uvicorn pydantic openai
```

LangChain dependencies:

```bash
.venv/bin/pip install langchain langchain-openai
```

LangGraph dependencies:

```bash
.venv/bin/pip install langgraph langgraph-checkpoint-sqlite
```

## Run server

```bash
.venv/bin/python run_server.py
```

This starts both:
- API server on `127.0.0.1:2731`
- Chainlit UI on `127.0.0.1:2732`

Development reboot:
- Call `POST http://127.0.0.1:2731/admin/reboot` to restart both processes.
- The request works when launched via `run_server.py` (it provides the supervisor trigger path).

Optional override:

```bash
SHEAF_PORT=9000 SHEAF_CHAINLIT_PORT=9001 .venv/bin/python run_server.py
```

## Run CLI loop

```bash
.venv/bin/python cli/chat_loop.py
```

Default target: `http://127.0.0.1:2731`

CLI commands:
- `/new` create and switch to a new chat
- `/list` list existing chats
- `/use <chat_id>` switch active chat
- `/quit` exit the loop

## Run Chainlit UI

Install Chainlit in your venv:

```bash
.venv/bin/pip install chainlit
```

Start web UI:

```bash
.venv/bin/chainlit run chainlit_app.py -w --port 2732
```

Then open the local URL shown by Chainlit (use `http://127.0.0.1:2732` if using `run_server.py` defaults).

Optional API target override for Chainlit:

```bash
SHEAF_API_BASE_URL=http://127.0.0.1:2731 .venv/bin/chainlit run chainlit_app.py -w --port 2732
```

Chainlit UI behavior in this repo:
- `Enter` sends message
- `Shift+Enter` inserts newline
- Left sidebar lists chats from the API and lets you click to switch chats
  - Switching is handled server-side via Chainlit window messages (more reliable than simulated typing)
- Sidebar includes a `Reboot` button that triggers `POST /admin/reboot`
  - Reboot is one-click (no confirmation prompt)

## Data layout

Runtime chat/checkpoint data is written under `data/`:

- `data/chats/<chat_id>/chat.json`
- `data/chats/<chat_id>/checkpoints/langgraph.sqlite`

Checkpoint content:
- LangGraph stores state snapshots in SQLite for each chat `thread_id`.
- State includes message history plus rolling compaction fields (for example `rolling_summary`).

Tool I/O output:
- Agent can call `write_note` to write files under `data/notes/<subdirs...>`.
- Agent can call `list_notes` to list directories/files under `data/notes`.
- Agent can call `read_note` to read whole files or line ranges under `data/notes`.
- Paths escaping `data/notes` are rejected.

Message indexing model:
- Messages are read from LangGraph state and exposed with a zero-based `index`.
- Index `0` is the first message in that chat transcript.
- `latest_checkpoint_id` comes from the current LangGraph checkpoint for that `thread_id`.
- Range API uses indices (`start` inclusive, `end` exclusive).

Lazy chat creation behavior:
- Empty chats are not persisted to `data/chats/` immediately.
- A chat appears in `GET /chats` after its first message is sent and checkpointed.

`data/` contents are gitignored.

## Next steps

- Tune compaction policy and add tests around summary quality/recall

## Model properties and compaction tuning

Model limits are resolved from `src/sheaf/llm/model_properties.py` and exposed by the dispatcher.

Useful env overrides:
- `SHEAF_MODEL_CONTEXT_WINDOW_TOKENS`
- `SHEAF_MODEL_MAX_OUTPUT_TOKENS`
- `SHEAF_MODEL_RESERVED_OUTPUT_TOKENS`
- `SHEAF_MODEL_SAFETY_MARGIN_TOKENS`
- `SHEAF_COMPACTION_TRIGGER_RATIO`
- `SHEAF_COMPACTION_TARGET_RATIO`
- `SHEAF_COMPACTION_RECENT_MESSAGES`

## Tools

Current tool support:
- `write_note(relative_path, content, overwrite=True)`
  - Writes UTF-8 text under `data/notes`.
  - Creates parent directories as needed.
  - Rejects paths outside `data/notes`.
- `list_notes(relative_dir=".", recursive=False)`
  - Lists entries under `data/notes` (optionally recursive).
- `read_note(relative_path, start_line=0, end_line=0)`
  - Reads UTF-8 file content under `data/notes`.
  - Supports 1-based line range reads (`start_line` inclusive, `end_line` exclusive).
