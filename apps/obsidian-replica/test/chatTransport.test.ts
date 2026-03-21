import test from "node:test";
import assert from "node:assert/strict";

import { decodeChatTransportEvent } from "../src/chat/transport.js";

test("decodeChatTransportEvent handles assistant tokens", () => {
  const event = decodeChatTransportEvent({
    type: "assistant_token",
    queue_id: 42,
    chunk: "hello",
  });

  assert.deepEqual(event, {
    type: "assistant_token",
    queueID: 42,
    chunk: "hello",
  });
});

test("decodeChatTransportEvent handles turn events", () => {
  const event = decodeChatTransportEvent({
    type: "turn_event",
    queue_id: 9,
    event: "thinking_trace",
    trace: "streamed_25_chunks",
    trace_kind: "operational",
    payload: { size: 25 },
  });

  assert.deepEqual(event, {
    type: "turn_event",
    queueID: 9,
    event: "thinking_trace",
    trace: "streamed_25_chunks",
    traceKind: "operational",
    payload: { size: 25 },
  });
});

test("decodeChatTransportEvent preserves fatal error metadata", () => {
  const event = decodeChatTransportEvent({
    type: "error",
    message: "Queue failed",
    queue_id: 17,
    fatal: true,
  });

  assert.deepEqual(event, {
    type: "error",
    message: "Queue failed",
    queueID: 17,
    fatal: true,
  });
});
