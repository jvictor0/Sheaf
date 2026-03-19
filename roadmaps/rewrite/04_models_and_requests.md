# Models And Requests

## Purpose

Stores model catalog metadata and per-request records for observability and replay.

## `models` table

```sql
CREATE TABLE models (
    name TEXT PRIMARY KEY,                  -- canonical model id, e.g. gpt-4.1
    provider TEXT NOT NULL,                 -- e.g. openai, anthropic
    api_model_id TEXT NOT NULL,             -- provider-specific model id
    is_local INTEGER NOT NULL DEFAULT 0,    -- 0=false, 1=true
    local_url TEXT NULL,                    -- endpoint for local model runtime
    context_window_tokens INTEGER NULL,
    max_output_tokens INTEGER NULL,
    metadata_json TEXT NULL,                -- provider/model extras
    created_at TEXT NOT NULL,
    updated_at TEXT NOT NULL
);
```

## `requests` table

```sql
CREATE TABLE requests (
    id TEXT PRIMARY KEY,                    -- UUID
    turn_id TEXT NOT NULL,
    model_name TEXT NOT NULL,
    request_json TEXT NOT NULL,             -- request payload sent to model
    response_json TEXT NULL,                -- full response payload
    error_text TEXT NULL,
    input_tokens INTEGER NULL,
    output_tokens INTEGER NULL,
    latency_ms INTEGER NULL,
    created_at TEXT NOT NULL,
    completed_at TEXT NULL,
    FOREIGN KEY (turn_id) REFERENCES turns(id) ON DELETE CASCADE,
    FOREIGN KEY (model_name) REFERENCES models(name)
);
```

## Indexes

```sql
CREATE INDEX idx_requests_turn_created
    ON requests(turn_id, created_at);

CREATE INDEX idx_requests_model_created
    ON requests(model_name, created_at);
```

## Notes

- `turns.model_name` should match the final model that produced that turn.
- `requests.model_name` captures model per API call (useful if retries or fallback models happen).
- `request_json` should request streaming whenever available from the target model/provider.
- Reasoning-capable calls should request thinking tokens and thinking trace outputs.
- First-pass non-local model entries include `gpt-5-mini` and `gpt-5.4`.
