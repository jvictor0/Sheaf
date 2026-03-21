# REST API Catalog

## Scope

Defines the simple non-streaming REST endpoints currently required in addition to chat entry.

## `GET /threads`

Returns non-archived threads.

Suggested response fields per thread:
- `id`
- `prev_thread_id`
- `start_turn_id`
- `is_archived`
- `tail_turn_id`
- `created_at`
- `updated_at`

Query behavior:
- Server filters with `is_archived = 0`.

## `GET /models`

Returns all models.

Suggested response fields per model:
- `name`
- `provider`
- `api_model_id`
- `is_local`
- `local_url`
- `context_window_tokens`
- `max_output_tokens`
- `metadata_json`

## `POST /models/updateLocalModelList`

Refreshes local model entries by querying the local Ollama runtime.

Behavior:
- Uses `ollama list` and/or equivalent Ollama REST API.
- Upserts local models in `models` table (`is_local = 1`).
- Removes or deactivates stale local rows that are no longer present.
- Returns updated local model list (or count + entries).

## `POST /threads/{thread_id}/archive`

Archives a thread by setting `is_archived = 1`.

Behavior:
- Idempotent if thread is already archived.
- Archived thread is excluded from default `GET /threads` results.

## `POST /threads/{thread_id}/unarchive`

Unarchives a thread by setting `is_archived = 0`.

Behavior:
- Idempotent if thread is already unarchived.
- Thread appears again in `GET /threads` results.

## Notes

- Includes read-only listing endpoints plus archive state mutation endpoints.
- Pagination/filtering can be added later if thread/model count grows.
