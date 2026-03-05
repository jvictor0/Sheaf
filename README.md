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
- Tool calling is enabled for controlled note writes under the configured tome directory

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
    "api_key": "YOUR_KEY_HERE"
  }
}
```

Notes:
- `.secrets.json` is gitignored and must never be committed.
- API keys are loaded from the `secrets_file` configured in `sheaf_server.config`.

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

`run_server.py` reads runtime settings from `sheaf_server.config` under `server`:
- `server.host`
- `server.api_port`
- `server.chainlit_port`
It also supports:
- `data_dir` (default: `data`)
- `secrets_file` (default: `.secrets.json`)
- `tome_dir` (default: `data/notes`; supports `~` expansion, e.g. `"~/AgentData/Sheaf"`)
- `llm.provider` (currently `openai`)
- `llm.openai_model` (default: `gpt-4.1-mini`)
- `llm.model_limits` and `llm.compaction` tuning blocks

Example:

```json
{
  "server": {
    "host": "127.0.0.1",
    "api_port": 2731,
    "chainlit_port": 2732
  }
}
```

This starts both using those configured values:
- API server on `http://<server.host>:<server.api_port>`
- Chainlit UI on `http://<server.host>:<server.chainlit_port>`

Development reboot:
- Call `POST http://<server.host>:<server.api_port>/admin/reboot` to restart both processes.
- The request works when launched via `run_server.py` (it provides the supervisor trigger path).

To also start the Zulip bot from `run_server.py`, set `"zulip_enabled": true` in `sheaf_server.config`.

Server runtime config policy:
- Use the config file only.
- Do not use environment variables to set server host/ports.

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

Then open the local URL shown by Chainlit (or `http://<server.host>:<server.chainlit_port>`).

## Run Zulip poll bot

Script: `scripts/zulip_poll_bot.py`

Purpose:
- Use Zulip event queues (`/register` + `/events`) for long-poll message delivery
- Send each message content to Sheaf (`POST /chats/{chat_id}/messages`)
- Post Sheaf response back to Zulip
- Persist progress and per-message processing status in SQLite

Install/setup:

```bash
.venv/bin/pip install -e .
cp sheaf_server.config.example sheaf_server.config
```

Config file:
- Default path: `sheaf_server.config`
- Example template: `sheaf_server.config.example`
- Required keys:
  - `zulip_enabled` (`true` = Zulip enabled / auto-start with `run_server.py`)
  - `zulip_site`
  - `zulip_bot_email`
  - `zulip_bot_api_key`
- Required for `run_server.py` network binding:
  - `server.host`
  - `server.api_port`
  - `server.chainlit_port`
- Optional keys:
  - `sheaf_api_base_url` (default: `http://127.0.0.1:2731`)
  - `sheaf_chat_id` (optional fixed chat override)
  - if `sheaf_chat_id` is empty, chat IDs are derived from Zulip context:
  - streams: `zulip-stream-<stream_id>-<stream-name-slug>`
  - DMs: `zulip-dm-<recipient_id>`
  - `state_db_path` (default: `data/zulip_bot_state.sqlite3`)
  - `poll_seconds` (default: `2.0`, retry/backoff delay after failures)
  - `batch_size` (default: `100`)
  - `narrow` (JSON list, default: mention-only)
  - `process_backlog_on_first_run` (default: `false`)
  - `false`: first run starts from newest message
  - `true`: first run starts from oldest available messages

Run:

```bash
.venv/bin/python scripts/zulip_poll_bot.py --config sheaf_server.config
```

Reliability behavior:
- `last_message_id` checkpoint is persisted in SQLite.
- `last_event_id` is tracked for current event queue progress.
- Every message ID is tracked in `processed_messages` with status (`processing`, `done`, `failed`).
- On failures, message remains retryable and `last_message_id` is not advanced past it.
- Event queue expiration is handled by re-registering, then catching up from `last_message_id`.
- Duplicate processing can still happen in crash windows, but messages are not lost.

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
- Agent can call `write_note` to write files under the configured `tome_dir`.
- Agent can call `list_notes` to list directories/files under the configured `tome_dir`.
- Agent can call `read_note` to read whole files or line ranges under the configured `tome_dir`.
- Paths escaping `tome_dir` are rejected.

Message indexing model:
- Messages are read from LangGraph state and exposed with a zero-based `index`.
- Index `0` is the first message in that chat transcript.
- `latest_checkpoint_id` comes from the current LangGraph checkpoint for that `thread_id`.
- Range API uses indices (`start` inclusive, `end` exclusive).

Lazy chat creation behavior:
- Empty chats are not persisted to `data/chats/` immediately.
- A chat appears in `GET /chats` after its first message is sent and checkpointed.

`data/` contents are gitignored.

## iOS LaTeX rendering

The iOS client renders math with MathJax SVG via a hidden WebKit worker:

- Worker file: `ios/SheafClient/SheafClient/Resources/MathJax/mathjax-worker.html`
- Swift render service: `ios/SheafClient/SheafClient/Services/MathJaxRenderService.swift`
- Math view/layout: `ios/SheafClient/SheafClient/Views/MathFormulaView.swift`

Recent fixes:
- The JS bridge uses a synchronous `renderMath(...)` result object (not a Promise) so `evaluateJavaScript` can decode it reliably.
- SVGs are rendered at intrinsic size (no forced `width: 100%`), reducing oversized/cropped glyphs.
- Baseline/depth metadata is used to avoid clipping lower portions of inline glyphs.
- Block math is horizontally scrollable in message bubbles.
- Math cache keys are versioned (`MathCacheKey`) so layout/render metric changes invalidate stale cached assets.

Supported math delimiters in chat text:
- Inline: `$...$`, `\\(...\\)`
- Block: `$$...$$`, `\\[...\\]`
- Fenced code blocks with math languages: ```` ```math ```` / ```` ```latex ```` / ```` ```tex ````

Note:
- Expressions not wrapped in supported delimiters are treated as normal text.

## Next steps

- Tune compaction policy and add tests around summary quality/recall

## Model properties and compaction tuning

Model limits are resolved from `src/sheaf/llm/model_properties.py` and exposed by the dispatcher.

Configuration-file tuning:
- `llm.model_limits.context_window_tokens`
- `llm.model_limits.max_output_tokens`
- `llm.model_limits.reserved_output_tokens`
- `llm.model_limits.safety_margin_tokens`
- `llm.compaction.trigger_ratio`
- `llm.compaction.target_ratio`
- `llm.compaction.recent_messages_to_keep`

## Tools

Current tool support:
- `write_note(relative_path, content, overwrite=True)`
  - Writes UTF-8 text under the configured `tome_dir`.
  - Creates parent directories as needed.
  - Rejects paths outside `tome_dir`.
- `list_notes(relative_dir=".", recursive=False)`
  - Lists entries under `tome_dir` (optionally recursive).
- `read_note(relative_path, start_line=0, end_line=0)`
  - Reads UTF-8 file content under `tome_dir`.
  - Supports 1-based line range reads (`start_line` inclusive, `end_line` exclusive).
