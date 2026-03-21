# Turn Execution And Commit

## Scope

Defines how a queued message becomes a committed turn.

## Durable Queue First

When message arrives from websocket:
1. Insert into `message_queue` with:
   - `thread_id`
   - `message_text`
   - `sender`
   - `in_response_to_turn_id`
   - timestamp data
2. Return durable acknowledgment to client.
3. Begin execution attempt.

## Execution Stream

Before model call:
- Evaluate context size deterministically outside LLM.
- If oversized, emit `context_compaction` event and compact.
- Persist compacted context into the turn `turn_context` that is later committed.
- For reasoning-capable model calls, request thinking tokens and thinking trace output.
- Use streaming mode for model requests whenever the model/provider supports it.

While executing:
- Stream tool-use events.
- Stream thinking/token events.
- Stream assistant message chunks.

Streaming is best effort and not ledger-authoritative until commit succeeds.

During execution:
- Persist tool call events and tool result events.
- Persist one or more thinking traces for the turn.

## Commit Transaction

Finalization is one DB transaction:
1. Read current thread tail.
2. Compare tail to `in_response_to_turn_id` (CAS precondition).
3. If match, append new committed turn row.
4. Re-check / enforce same CAS expectation at commit boundary.
5. Update `threads.tail_turn_id` to new turn ID.
6. Delete queue row for this request.
7. Commit.

After commit:
- Send final event containing committed turn ID.

## CAS Failure Semantics

If precondition fails at start or end:
- Abort commit.
- Do not append new ledger turn.
- Return execution conflict error to client.
- Client reconnects and resynchronizes from committed ledger.

## Websocket Optionality

If websocket is unavailable during execution:
- Continue execution and commit to DB.
- Skip live stream delivery.
- On reconnect, client receives committed state via sync provisioning.
