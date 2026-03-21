import test from "node:test";
import assert from "node:assert/strict";

import { ChatStore, applyCommittedTurn, createChatSession, dropQueueArtifacts, dropUncommittedArtifacts } from "../src/chat/store.js";
import { summarizeToolCall } from "../src/chat/toolSummary.js";

import type { ChatCommittedTurn, ChatThreadSummary } from "../src/types.js";

function threadSummary(): ChatThreadSummary {
  return {
    thread_id: "thread-1",
    name: "Thread 1",
    prev_thread_id: null,
    start_turn_id: null,
    is_archived: false,
    tail_turn_id: null,
    created_at: null,
    updated_at: null,
  };
}

function assistantTurn(): ChatCommittedTurn {
  return {
    id: "assistant-1",
    thread_id: "thread-1",
    prev_turn_id: "user-1",
    speaker: "assistant",
    message_text: "Answer",
    model_name: "gpt-5.4",
    created_at: null,
    tool_calls: [
      {
        id: "tool-1",
        name: "read_note",
        args: { relative_path: "folder/note.md" },
        result: "note text",
        isError: false,
      },
    ],
  };
}

test("transcript ordering is committed then pending then streaming", () => {
  const session = createChatSession(threadSummary());
  session.committedTurns.push({
    id: "user-1",
    thread_id: "thread-1",
    prev_turn_id: null,
    speaker: "user",
    message_text: "Hello",
    model_name: null,
    created_at: null,
    tool_calls: [],
  });
  session.pendingSends.push({
    clientMessageID: "pending-1",
    text: "Pending",
    responseToTurnID: "assistant-1",
    localMessageID: "local-pending-1",
    queueID: null,
  });
  session.streamingByQueue[7] = { queueID: 7, text: "Typing" };

  const store = new ChatStore();
  store.setThreads([threadSummary()]);
  store.openConversation(threadSummary());
  store.replaceSession("thread-1", session);

  const items = store.getSession("thread-1")?.transcriptItems ?? [];
  assert.deepEqual(
    items.map((item) => item.kind),
    ["committed", "pending", "streaming"],
  );
});

test("fresh sessions do not treat thread summary tail as a known committed tail", () => {
  const session = createChatSession({
    ...threadSummary(),
    tail_turn_id: "server-tail-1",
  });

  assert.equal(session.lastCommittedTurnID, null);
});

test("committed user turn consumes matching pending send", () => {
  const session = createChatSession(threadSummary());
  session.pendingSends.push({
    clientMessageID: "pending-1",
    text: "Hello",
    responseToTurnID: null,
    localMessageID: "local-pending-1",
    queueID: null,
  });

  applyCommittedTurn(session, {
    id: "user-1",
    thread_id: "thread-1",
    prev_turn_id: null,
    speaker: "user",
    message_text: "Hello",
    model_name: null,
    created_at: null,
    tool_calls: [],
  });

  assert.equal(session.pendingSends.length, 0);
});

test("dropUncommittedArtifacts clears pending and streaming state", () => {
  const session = createChatSession(threadSummary());
  session.pendingSends.push({
    clientMessageID: "pending-1",
    text: "Hello",
    responseToTurnID: null,
    localMessageID: "local-pending-1",
    queueID: null,
  });
  session.streamingByQueue[1] = { queueID: 1, text: "Thinking" };
  session.thinkingActive = true;
  session.statusMessage = "Thinking…";

  dropUncommittedArtifacts(session);

  assert.equal(session.pendingSends.length, 0);
  assert.deepEqual(session.streamingByQueue, {});
  assert.equal(session.thinkingActive, false);
});

test("dropQueueArtifacts only clears matching queue state", () => {
  const session = createChatSession(threadSummary());
  session.pendingSends.push({
    clientMessageID: "pending-1",
    text: "First",
    responseToTurnID: null,
    localMessageID: "local-pending-1",
    queueID: 7,
  });
  session.pendingSends.push({
    clientMessageID: "pending-2",
    text: "Second",
    responseToTurnID: null,
    localMessageID: "local-pending-2",
    queueID: 8,
  });
  session.streamingByQueue[7] = { queueID: 7, text: "One" };
  session.streamingByQueue[8] = { queueID: 8, text: "Two" };
  session.thinkingActive = true;
  session.statusMessage = "Assistant is responding…";

  dropQueueArtifacts(session, 7);

  assert.deepEqual(
    session.pendingSends.map((pending) => pending.clientMessageID),
    ["pending-2"],
  );
  assert.deepEqual(Object.keys(session.streamingByQueue), ["8"]);
  assert.equal(session.thinkingActive, true);
  assert.equal(session.statusMessage, "Assistant is responding…");
});

test("tool summary keeps only file-safe label", () => {
  assert.equal(
    summarizeToolCall({
      id: "tool-1",
      name: "write_note",
      args: { relative_path: "daily/tasks.md", content: "do not show this" },
      result: "ignored",
      isError: false,
    }),
    "Wrote daily/tasks.md",
  );
});

test("tool summary strips server vault root and recognizes create_file", () => {
  assert.equal(
    summarizeToolCall({
      id: "tool-2",
      name: "create_file",
      args: { path: "data/vaults/test/toaster.md", content: "do not show this" },
      result: "ignored",
      isError: false,
    }),
    "Created toaster.md",
  );
});

test("tool summary strips server vault root for patches", () => {
  assert.equal(
    summarizeToolCall({
      id: "tool-3",
      name: "apply_patch",
      args: { path: "data/vaults/test/toaster.md", patch: "do not show this" },
      result: "ignored",
      isError: false,
    }),
    "Applied patch to toaster.md",
  );
});

test("assistant transcript includes tool call summary before the turn", () => {
  const session = createChatSession(threadSummary());
  applyCommittedTurn(session, assistantTurn());

  assert.deepEqual(
    session.transcriptItems.map((item) => item.kind),
    ["tool_call", "committed"],
  );
});
