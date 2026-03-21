# Issues

## Legacy iOS REST metadata endpoints drift from the live server

The iOS client model layer still defines `GET /threads/{id}/metadata` and
`GET /threads/{id}/messages`, but the current Python server does not expose
those routes. The live chat history contract is websocket handshake replay plus
`committed_turn` frames.

Status: `deferred`

Next Action: `Track the follow-up in side quest legacy_chat_rest_contract and decide later whether the server should restore the legacy REST endpoints or the iOS client should retire the stale helpers.`

## Tool call summary needs a stable filename extraction rule

The user-facing requirement is to show only the filename for read, write, and
patch tool calls. Current tool payloads may include keys such as
`relative_path`, `relative_dir`, or other tool-specific arguments. The planning
docs need a clear filename-only rule so the Obsidian pane does not accidentally
surface full paths or file contents.

Status: `deferred`

Next Action: `Track the follow-up in side quest tool_call_summary_contract and define the authoritative privacy-safe filename/path summarization rules for file-oriented tool payloads.`


## Patch tool contract mismatches OpenAI-style agent behavior

The current server-side `apply_patch` tool expects a unified diff hunk format,
but OpenAI-style coding agents commonly emit structured patch envelopes such as
`*** Begin Patch` / `*** Update File` / `*** End Patch`. This mismatch caused
repeated failed tool calls in the `Toaster` thread even when the intended file
edit was otherwise straightforward.

Status: `deferred`

Next Action: `Track the follow-up in side quest chat_patch_tool_contract and decide whether to adopt an OpenAI-native patch contract, rename the unified-diff tool, or add a compatibility adapter.`

## Fatal queue error cleanup drops unrelated in-flight work

The transport now preserves `queue_id` and `fatal`, but the service still
handles any fatal queue error by calling `dropUncommittedArtifacts(session)`.
That helper clears every pending send and every streaming buffer in the active
thread, even though the protocol and in-memory state are queue-scoped. If the
user has multiple sends in flight, a fatal failure for one queue item erases
unrelated pending or streaming UI for the others.

Status: `completed`

Next Action: `No action. Verified in review that queue-scoped fatal errors now call dropQueueArtifacts(session, event.queueID) instead of dropUncommittedArtifacts. Test coverage confirms queue-scoped cleanup preserves unrelated in-flight work.`

## Composer can submit before replay finishes

The conversation composer stays enabled as soon as the conversation view mounts,
and `sendMessage(...)` only checks for an active thread plus an open websocket.
That means the user can submit before `handshake_ready` arrives, while replay or
startup drain is still establishing the authoritative `lastCommittedTurnID`.
Early sends can therefore race the initial history load and trigger avoidable
`execution_conflict` failures or attach to a stale tail.

Status: `completed`

Next Action: `No action. Verified in review that sendMessage checks connectionState !== "live" and returns false, and connectionState only becomes "live" after handshake_ready. The view also disables the composer until connectionState is live. Test coverage confirms sends are blocked during replay.`

## Fatal chat error frames cannot clear pending or streaming UI state

The transport decoder currently collapses every websocket `error` frame down to
just a message string, discarding the protocol's `queue_id` and `fatal` fields.
That means the service cannot distinguish a fatal queue/send failure from a
transient transport message, so it leaves pending sends and streaming buffers in
place instead of performing the fatal-error cleanup required by the quest.

Status: `completed`

Next Action: `No action. Verified in review that websocket error decoding now preserves queue-scoped fatal metadata and the fatal cleanup path is covered by tests.`

## Execution-conflict resync can reopen a thread after the user already left it

On `execution_conflict`, the service schedules an uncancelled `setTimeout`
which directly calls `openThread(...)` later. If the user hits Back or closes
the pane before that timer fires, the delayed callback still re-enters the old
thread and reconnects anyway, violating the planned disconnect-on-leave
behavior and risking surprising navigation during thread switching.

Status: `completed`

Next Action: `No action. Verified in review that conflict recovery now routes through the cancellable reconnect path and leaving before the timer fires is covered by tests.`
