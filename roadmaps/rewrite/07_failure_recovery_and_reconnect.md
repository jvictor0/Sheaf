# Failure Recovery And Reconnect

## Core Principle

Ledger truth is database state, not in-flight stream output.

If anything fails after partial streaming, client reconnects and rebuilds from committed turns.

## Expected Failure Cases

- websocket disconnect mid-stream
- execution crash after partial events
- CAS tail mismatch
- model/tool execution error

## Client Recovery Behavior

On reconnect, client sends last committed turn ID it trusts.

Server then:
- streams all turns after that ID, or
- last 20 turns if no ID is provided

Client discards uncommitted local partial stream artifacts and rerenders from server-provided ledger.

## Server Recovery Behavior

- Do not treat stream delivery as commit success.
- Commit path determines whether a turn is official.
- If turn commit fails, require client resync.
- If execution started before disconnect, server may still complete commit.

## Consistency Contract

- Conflicts are resolved by CAS against thread tail.
- Final client-visible consistency is achieved by reconnect + replay from committed state.
