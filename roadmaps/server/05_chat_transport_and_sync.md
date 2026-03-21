# Chat Transport And Sync

## Scope

Defines the client entry API and websocket synchronization behavior.

## Entry API

- Server exposes REST API for entering chat by thread name.
- Entry call upgrades to websocket for lightweight streaming JSON-RPC.
- Websocket is the primary transport for streamed execution events.

## Handshake Input

Client sends one of:
- `known_tail_turn_id = null` (client has no known ledger position)
- `known_tail_turn_id = <turn_id>` (last confirmed turn the client has)

## Handshake Provisioning Behavior

On connection startup:
- If `known_tail_turn_id` is present, stream all committed turns after that ID.
- If `known_tail_turn_id` is null, stream the last 20 turns for that thread.
- Include context budget metadata in handshake provisioning:
  - `context_size` (current context usage for active turn/model session)
  - `max_context_size` (model context limit for selected model)

## Outstanding Request Drain

Before server sends connection-established confirmation:
- If there is an outstanding request for this chat, attempt to execute it.
- Stream resulting provisioning output to client as part of startup transfer.
- Regardless of success or failure, delete the outstanding request before final confirmation.
- Invariant at end of handshake: no outstanding request remains for this chat.

## Live Message Submission

After connection is established, client sends JSON messages including:
- user message text
- model selection
- user config payload
- `in_response_to_turn_id` (nullable for first message in thread)

Server responses for each message include context budget metadata so client can show remaining context:
- `context_size`
- `max_context_size`

Server first persists the request to durable queue, then confirms durability to client.

## Notes

- Streaming response envelope shape will be specified separately.
- This doc defines sequencing and consistency behavior only.
