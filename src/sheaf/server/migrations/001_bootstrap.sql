-- Bootstrap schema for rewritten server runtime.
-- This is the single schema source-of-truth for now.

CREATE TABLE IF NOT EXISTS threads (
    id TEXT PRIMARY KEY,
    name TEXT NOT NULL,
    prev_thread_id TEXT NULL,
    start_turn_id TEXT NULL,
    is_archived INTEGER NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL,
    tail_turn_id TEXT NULL,
    FOREIGN KEY (prev_thread_id) REFERENCES threads(id),
    FOREIGN KEY (start_turn_id) REFERENCES turns(id),
    FOREIGN KEY (tail_turn_id) REFERENCES turns(id)
);

CREATE TABLE IF NOT EXISTS turns (
    id TEXT PRIMARY KEY,
    thread_id TEXT NOT NULL,
    prev_turn_id TEXT NULL,
    speaker TEXT NOT NULL,
    message_text TEXT NOT NULL,
    turn_context TEXT NULL,
    stats_json TEXT NULL,
    model_name TEXT NULL,
    created_at TEXT NOT NULL,
    FOREIGN KEY (thread_id) REFERENCES threads(id) ON DELETE CASCADE,
    FOREIGN KEY (prev_turn_id) REFERENCES turns(id),
    FOREIGN KEY (model_name) REFERENCES models(name),
    CHECK (prev_turn_id IS NULL OR prev_turn_id <> id)
);

CREATE TABLE IF NOT EXISTS message_queue (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    thread_id TEXT NOT NULL,
    response_to_turn_id TEXT NULL,
    sender TEXT NOT NULL,
    message_text TEXT NOT NULL,
    model_name TEXT NOT NULL,
    client_message_id TEXT NULL,
    enqueued_at TEXT NOT NULL,
    attempts INTEGER NOT NULL DEFAULT 0,
    available_at TEXT NOT NULL,
    locked_by TEXT NULL,
    locked_at TEXT NULL,
    last_error TEXT NULL,
    FOREIGN KEY (thread_id) REFERENCES threads(id) ON DELETE CASCADE,
    FOREIGN KEY (response_to_turn_id) REFERENCES turns(id)
);

CREATE TABLE IF NOT EXISTS queue_errors (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    queue_id INTEGER NOT NULL,
    thread_id TEXT NOT NULL,
    response_to_turn_id TEXT NULL,
    model_name TEXT NOT NULL,
    message_text TEXT NOT NULL,
    attempts INTEGER NOT NULL,
    error_type TEXT NOT NULL,
    error_text TEXT NOT NULL,
    failure_stage TEXT NOT NULL,
    created_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS turn_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    turn_id TEXT NOT NULL,
    event_type TEXT NOT NULL,
    tool_name TEXT NULL,
    tool_args_json TEXT NULL,
    payload_json TEXT NULL,
    created_at TEXT NOT NULL,
    FOREIGN KEY (turn_id) REFERENCES turns(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS thinking_traces (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    turn_id TEXT NOT NULL,
    sequence_no INTEGER NOT NULL,
    trace_text TEXT NOT NULL,
    created_at TEXT NOT NULL,
    FOREIGN KEY (turn_id) REFERENCES turns(id) ON DELETE CASCADE
);

CREATE TABLE IF NOT EXISTS models (
    name TEXT PRIMARY KEY,
    provider TEXT NOT NULL,
    api_model_id TEXT NOT NULL,
    is_local INTEGER NOT NULL DEFAULT 0,
    local_url TEXT NULL,
    context_window_tokens INTEGER NULL,
    max_output_tokens INTEGER NULL,
    metadata_json TEXT NULL,
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE TABLE IF NOT EXISTS requests (
    id TEXT PRIMARY KEY,
    turn_id TEXT NULL,
    model_name TEXT NOT NULL,
    request_json TEXT NOT NULL,
    response_json TEXT NULL,
    error_text TEXT NULL,
    input_tokens INTEGER NULL,
    output_tokens INTEGER NULL,
    latency_ms INTEGER NULL,
    created_at TEXT NOT NULL,
    completed_at TEXT NULL,
    FOREIGN KEY (turn_id) REFERENCES turns(id) ON DELETE CASCADE,
    FOREIGN KEY (model_name) REFERENCES models(name)
);

CREATE TABLE IF NOT EXISTS visible_directories (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    path TEXT NOT NULL UNIQUE,
    access_mode TEXT NOT NULL CHECK (access_mode IN ('read_only', 'read_write')),
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_turns_thread_created ON turns(thread_id, created_at);
CREATE INDEX IF NOT EXISTS idx_turns_prev_turn ON turns(prev_turn_id);
CREATE INDEX IF NOT EXISTS idx_threads_is_archived ON threads(is_archived, updated_at);
CREATE INDEX IF NOT EXISTS idx_queue_thread_enqueued ON message_queue(thread_id, enqueued_at);
CREATE INDEX IF NOT EXISTS idx_queue_available ON message_queue(available_at, locked_by);
CREATE INDEX IF NOT EXISTS idx_queue_response_to_turn ON message_queue(response_to_turn_id);
CREATE INDEX IF NOT EXISTS idx_queue_errors_created ON queue_errors(created_at);
CREATE INDEX IF NOT EXISTS idx_turn_events_turn_created ON turn_events(turn_id, created_at);
CREATE INDEX IF NOT EXISTS idx_turn_events_type_created ON turn_events(event_type, created_at);
CREATE INDEX IF NOT EXISTS idx_thinking_traces_turn_sequence ON thinking_traces(turn_id, sequence_no);
CREATE INDEX IF NOT EXISTS idx_requests_turn_created ON requests(turn_id, created_at);
CREATE INDEX IF NOT EXISTS idx_requests_model_created ON requests(model_name, created_at);
