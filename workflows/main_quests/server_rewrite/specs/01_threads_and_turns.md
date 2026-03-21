# Threads And Turns

## Purpose

Defines conversation structure as a linked list of turns per thread.

## `threads` table

Tracks thread identity and current tail turn.

```sql
CREATE TABLE threads (
    id TEXT PRIMARY KEY,                      -- UUID
    prev_thread_id TEXT NULL,                 -- parent thread when this is a fork
    start_turn_id TEXT NULL,                  -- turn in parent where fork started
    is_archived INTEGER NOT NULL DEFAULT 0,   -- 0=false, 1=true
    created_at TEXT NOT NULL,                -- ISO-8601 UTC timestamp
    updated_at TEXT NOT NULL,                -- ISO-8601 UTC timestamp
    tail_turn_id TEXT NULL,                  -- points to latest turn in thread
    FOREIGN KEY (prev_thread_id) REFERENCES threads(id),
    FOREIGN KEY (start_turn_id) REFERENCES turns(id),
    FOREIGN KEY (tail_turn_id) REFERENCES turns(id)
);
```

Notes:
- `tail_turn_id` is nullable so an empty thread is allowed.
- `prev_thread_id` and `start_turn_id` are both null for brand-new root threads.
- Forked threads set `prev_thread_id` to source thread and `start_turn_id` to fork point turn.
- `is_archived=1` hides thread from default thread listing.
- `updated_at` should be touched whenever new turns are appended.

## `turns` table

Stores each message/eventful assistant output as an immutable turn.

```sql
CREATE TABLE turns (
    id TEXT PRIMARY KEY,                      -- UUID
    thread_id TEXT NOT NULL,                  -- parent thread UUID
    prev_turn_id TEXT NULL,                   -- null for first turn
    speaker TEXT NOT NULL,
    message_text TEXT NOT NULL,
    turn_context TEXT NULL,                   -- serialized context for model input
    stats_json TEXT NULL,                     -- optional row-level stats (JSON string)
    model_name TEXT NULL,                     -- model used for assistant generation
    created_at TEXT NOT NULL,                 -- ISO-8601 UTC timestamp
    FOREIGN KEY (thread_id) REFERENCES threads(id) ON DELETE CASCADE,
    FOREIGN KEY (prev_turn_id) REFERENCES turns(id),
    FOREIGN KEY (model_name) REFERENCES models(name),
    CHECK (
        prev_turn_id IS NULL OR prev_turn_id <> id
    )
);
```

Notes:
- `prev_turn_id` links to prior turn and creates a chain.
- `model_name` is optional because user turns will not have a model.

## Indexes

```sql
CREATE INDEX idx_turns_thread_created
    ON turns(thread_id, created_at);

CREATE INDEX idx_turns_prev_turn
    ON turns(prev_turn_id);

CREATE INDEX idx_threads_prev_thread
    ON threads(prev_thread_id);

CREATE INDEX idx_threads_start_turn
    ON threads(start_turn_id);

CREATE INDEX idx_threads_is_archived
    ON threads(is_archived, updated_at);
```

## Recommended invariants (application-level)

Some constraints are difficult to enforce cleanly in pure SQLite without triggers:
- `prev_turn_id` should belong to the same `thread_id`.
- `threads.tail_turn_id` should belong to the same `thread_id`.
- `threads.start_turn_id` should belong to `threads.prev_thread_id` when set.
- New turn append should update thread tail atomically.

These should be enforced by transaction logic in the runner.

## Thread creation semantics

- New root thread flow:
  - client request may provide `thread_id = null`, `prev_thread_id = null`, `start_turn_id = null`
  - server allocates new `threads.id` and stores null fork fields
- Fork flow:
  - client provides source `prev_thread_id` and fork `start_turn_id`
  - server allocates new `threads.id`, sets fork metadata, and starts new branch chain
