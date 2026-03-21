import test from "node:test";
import assert from "node:assert/strict";

import { decodeCommittedTurn, decodeEnterChatResponse, decodeThreadsResponse } from "../src/chat/protocol.js";

test("decodeThreadsResponse preserves current server shape", () => {
  const threads = decodeThreadsResponse({
    threads: [
      {
        thread_id: "thread-1",
        name: "Daily notes",
        prev_thread_id: null,
        start_turn_id: null,
        is_archived: false,
        tail_turn_id: "tail-1",
        created_at: "2026-03-21T10:00:00Z",
        updated_at: "2026-03-21T12:00:00Z",
      },
    ],
  });

  assert.equal(threads[0]?.thread_id, "thread-1");
  assert.equal(threads[0]?.tail_turn_id, "tail-1");
});

test("decodeEnterChatResponse validates session fields", () => {
  const response = decodeEnterChatResponse({
    session_id: "session-1",
    websocket_url: "/ws/chat/session-1",
    accepted_protocol_version: 1,
  });

  assert.equal(response.session_id, "session-1");
  assert.equal(response.websocket_url, "/ws/chat/session-1");
});

test("decodeCommittedTurn keeps tool call metadata", () => {
  const turn = decodeCommittedTurn({
    id: "turn-1",
    thread_id: "thread-1",
    prev_turn_id: null,
    speaker: "assistant",
    message_text: "Done.",
    model_name: "gpt-5.4",
    created_at: "2026-03-21T12:00:00Z",
    tool_calls: [
      {
        id: "tool-1",
        name: "read_note",
        args: { relative_path: "notes/today.md" },
        result: "ignored",
        is_error: false,
      },
    ],
  });

  assert.equal(turn.tool_calls[0]?.name, "read_note");
  assert.deepEqual(turn.tool_calls[0]?.args, { relative_path: "notes/today.md" });
});
