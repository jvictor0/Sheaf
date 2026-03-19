# Message Queue

## Purpose

Queues inbound/outbound work for the runner to process asynchronously.

## `message_queue` table

```sql
CREATE TABLE message_queue (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    thread_id TEXT NOT NULL,
    response_to_turn_id TEXT NULL,          -- null if no specific parent turn
    sender TEXT NOT NULL,
    message_text TEXT NOT NULL,
    enqueued_at TEXT NOT NULL,              -- ISO-8601 UTC timestamp
    attempts INTEGER NOT NULL DEFAULT 0 CHECK (attempts >= 0),
    available_at TEXT NOT NULL,             -- for retry/backoff scheduling
    locked_by TEXT NULL,                    -- worker identifier
    locked_at TEXT NULL,
    last_error TEXT NULL,
    FOREIGN KEY (thread_id) REFERENCES threads(id) ON DELETE CASCADE,
    FOREIGN KEY (response_to_turn_id) REFERENCES turns(id)
);
```

## Indexes

```sql
CREATE INDEX idx_queue_thread_enqueued
    ON message_queue(thread_id, enqueued_at);

CREATE INDEX idx_queue_response_to_turn
    ON message_queue(response_to_turn_id);
```

## Processing model (intended)

- Poll `locked_by is null` with `available_at <= now`.
- Claim row by setting `locked_by`, `locked_at`.
- On success delete.
- On retryable failure increment `attempts`, set new `available_at`, revert to `queued`.
- On terminal failure delete

## Restart startup step

On server startup (after crash/restart), clear lock ownership before processing:

```sql
UPDATE message_queue
SET locked_by = NULL,
    locked_at = NULL;
```

This cancels any previously in-progress claim and makes rows runnable again under the single active server instance.
