import { CHAT_PROTOCOL_VERSION, decodeCommittedTurn } from "./protocol.js";

import type { ChatClosedEvent, ChatJSONValue, ChatTransportEvent, ReplicaPluginSettings } from "../types.js";

type EventHandler = (event: ChatTransportEvent) => void;

function isObject(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

function asString(value: unknown): string | null {
  return typeof value === "string" ? value : null;
}

function asNumber(value: unknown): number | null {
  return typeof value === "number" ? value : null;
}

function toJSONValue(value: unknown): ChatJSONValue | null {
  if (
    value === null ||
    typeof value === "string" ||
    typeof value === "number" ||
    typeof value === "boolean"
  ) {
    return value;
  }
  if (Array.isArray(value)) {
    return value.map((item) => toJSONValue(item) ?? null);
  }
  if (isObject(value)) {
    return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, toJSONValue(item) ?? null]));
  }
  return null;
}

export function decodeChatTransportEvent(payload: unknown): ChatTransportEvent | null {
  if (!isObject(payload) || !asString(payload.type)) {
    return null;
  }

  const type = asString(payload.type) ?? "";
  switch (type) {
    case "handshake_snapshot_begin":
      return { type: "handshake_begin", threadID: asString(payload.thread_id) };
    case "handshake_ready":
      return { type: "handshake_ready" };
    case "message_durable_ack":
      return {
        type: "durable_ack",
        queueID: asNumber(payload.queue_id) ?? -1,
        clientMessageID: asString(payload.client_message_id),
      };
    case "assistant_token":
      return {
        type: "assistant_token",
        queueID: asNumber(payload.queue_id) ?? -1,
        chunk: asString(payload.chunk) ?? "",
      };
    case "committed_turn":
      return { type: "committed_turn", turn: decodeCommittedTurn(payload.turn) };
    case "turn_finalized":
      return {
        type: "turn_finalized",
        queueID: asNumber(payload.queue_id) ?? -1,
        turnID: asString(payload.turn_id),
      };
    case "execution_conflict":
      return {
        type: "execution_conflict",
        queueID: asNumber(payload.queue_id) ?? -1,
        expectedTailTurnID: asString(payload.expected_tail_turn_id),
        actualTailTurnID: asString(payload.actual_tail_turn_id),
      };
    case "context_budget":
      return {
        type: "context_budget",
        context: {
          contextSize: asNumber(payload.context_size) ?? 0,
          maxContextSize: asNumber(payload.max_context_size) ?? 0,
        },
      };
    case "turn_event":
      return {
        type: "turn_event",
        queueID: asNumber(payload.queue_id) ?? -1,
        event: asString(payload.event) ?? "unknown",
        trace: asString(payload.trace),
        traceKind: asString(payload.trace_kind),
        payload: toJSONValue(payload.payload),
      };
    case "heartbeat":
      return { type: "heartbeat", intervalSeconds: asNumber(payload.interval_seconds) };
    case "error":
      return {
        type: "error",
        message: asString(payload.message) ?? "Unknown chat transport error",
        queueID: asNumber(payload.queue_id),
        fatal: payload.fatal === true,
      };
    default:
      return null;
  }
}

export class ChatTransportClient {
  private socket: WebSocket | null = null;
  private intentionalClose = false;

  constructor(private readonly settings: () => ReplicaPluginSettings) {}

  async connect(websocketPath: string, onEvent: EventHandler): Promise<void> {
    this.disconnect();
    this.intentionalClose = false;

    await new Promise<void>((resolve, reject) => {
      const socket = new WebSocket(this.websocketURL(websocketPath));
      this.socket = socket;

      socket.addEventListener("open", () => resolve(), { once: true });
      socket.addEventListener("error", () => reject(new Error("Chat websocket failed to connect")), { once: true });

      socket.addEventListener("message", (event) => {
        try {
          const raw = JSON.parse(String(event.data)) as unknown;
          const decoded = decodeChatTransportEvent(raw);
          if (decoded) {
            onEvent(decoded);
          }
        } catch (error) {
          onEvent({
            type: "error",
            message: error instanceof Error ? error.message : "Malformed websocket payload",
            queueID: null,
            fatal: true,
          });
        }
      });

      socket.addEventListener("close", (event) => {
        this.socket = null;
        if (this.intentionalClose) {
          return;
        }
        const closedEvent: ChatClosedEvent = {
          type: "closed",
          code: event.code,
          reason: event.reason || null,
        };
        onEvent(closedEvent);
      });
    });
  }

  disconnect(): void {
    this.intentionalClose = true;
    this.socket?.close();
    this.socket = null;
  }

  async submitMessage(args: {
    threadID: string;
    text: string;
    modelName: string;
    inResponseToTurnID: string | null;
    clientMessageID: string;
  }): Promise<void> {
    if (!this.socket) {
      throw new Error("Chat transport is not connected");
    }
    this.socket.send(
      JSON.stringify({
        protocol_version: CHAT_PROTOCOL_VERSION,
        type: "submit_message",
        thread_id: args.threadID,
        text: args.text,
        model_name: args.modelName,
        in_response_to_turn_id: args.inResponseToTurnID,
        client_message_id: args.clientMessageID,
      }),
    );
  }

  private websocketURL(path: string): string {
    if (/^wss?:\/\//.test(path)) {
      return path;
    }
    const baseUrl = new URL(this.settings().serverBaseUrl);
    baseUrl.protocol = baseUrl.protocol === "https:" ? "wss:" : "ws:";
    baseUrl.pathname = path;
    baseUrl.search = "";
    return baseUrl.toString();
  }
}
