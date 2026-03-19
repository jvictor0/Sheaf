# Turn Events

## Purpose

Captures detailed execution events for each turn:
- Tool calls
- Context management actions
- Internal orchestration milestones

## `turn_events` table

```sql
CREATE TABLE turn_events (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    turn_id TEXT NOT NULL,
    event_type TEXT NOT NULL,               -- e.g. tool_use, context_trim, model_request
    tool_name TEXT NULL,                    -- present when event_type is tool-related
    tool_args_json TEXT NULL,               -- serialized args
    payload_json TEXT NULL,                 -- generic event payload
    created_at TEXT NOT NULL,               -- ISO-8601 UTC timestamp
    FOREIGN KEY (turn_id) REFERENCES turns(id) ON DELETE CASCADE
);
```

## `thinking_traces` table

Stores intermediate reasoning traces emitted during a turn.

```sql
CREATE TABLE thinking_traces (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    turn_id TEXT NOT NULL,
    sequence_no INTEGER NOT NULL,             -- ordering within a turn
    trace_text TEXT NOT NULL,                 -- persisted thought/trace payload
    created_at TEXT NOT NULL,                 -- ISO-8601 UTC timestamp
    FOREIGN KEY (turn_id) REFERENCES turns(id) ON DELETE CASCADE
);
```

## Indexes

```sql
CREATE INDEX idx_turn_events_turn_created
    ON turn_events(turn_id, created_at);

CREATE INDEX idx_turn_events_type_created
    ON turn_events(event_type, created_at);

CREATE INDEX idx_thinking_traces_turn_sequence
    ON thinking_traces(turn_id, sequence_no);
```

## Notes

- Keep `payload_json` flexible so new event types do not require schema changes.
- Tool events can use both `tool_name` and `tool_args_json`; non-tool events can leave them null.
- Emit explicit context-compaction event when deterministic pre-LLM compaction runs.
- Persist each tool call and tool result as events; multiple calls per turn are expected.
- Persist multiple thinking traces per turn when the turn has iterative reasoning/tool loops.
