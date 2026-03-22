"use strict";
var __defProp = Object.defineProperty;
var __getOwnPropDesc = Object.getOwnPropertyDescriptor;
var __getOwnPropNames = Object.getOwnPropertyNames;
var __hasOwnProp = Object.prototype.hasOwnProperty;
var __export = (target, all) => {
  for (var name in all)
    __defProp(target, name, { get: all[name], enumerable: true });
};
var __copyProps = (to, from, except, desc) => {
  if (from && typeof from === "object" || typeof from === "function") {
    for (let key of __getOwnPropNames(from))
      if (!__hasOwnProp.call(to, key) && key !== except)
        __defProp(to, key, { get: () => from[key], enumerable: !(desc = __getOwnPropDesc(from, key)) || desc.enumerable });
  }
  return to;
};
var __toCommonJS = (mod) => __copyProps(__defProp({}, "__esModule", { value: true }), mod);

// src/main.ts
var main_exports = {};
__export(main_exports, {
  default: () => SheafObsidianReplicaPlugin
});
module.exports = __toCommonJS(main_exports);
var import_obsidian7 = require("obsidian");

// src/chat/api.ts
var import_obsidian = require("obsidian");

// src/chat/protocol.ts
var CHAT_PROTOCOL_VERSION = 1;
function isObject(value) {
  return typeof value === "object" && value !== null;
}
function asString(value) {
  return typeof value === "string" ? value : null;
}
function asBoolean(value) {
  return value === true;
}
function normalizeJSONValue(value) {
  if (value === null || typeof value === "string" || typeof value === "number" || typeof value === "boolean") {
    return value;
  }
  if (Array.isArray(value)) {
    return value.map((item) => normalizeJSONValue(item));
  }
  if (isObject(value)) {
    const output = {};
    for (const [key, item] of Object.entries(value)) {
      output[key] = normalizeJSONValue(item);
    }
    return output;
  }
  return String(value);
}
function decodeThreadSummary(value) {
  if (!isObject(value) || !asString(value.thread_id)) {
    throw new Error("Invalid thread summary");
  }
  const threadID = asString(value.thread_id) ?? "";
  return {
    thread_id: threadID,
    name: asString(value.name) ?? threadID,
    prev_thread_id: asString(value.prev_thread_id),
    start_turn_id: asString(value.start_turn_id),
    is_archived: asBoolean(value.is_archived),
    tail_turn_id: asString(value.tail_turn_id),
    created_at: asString(value.created_at),
    updated_at: asString(value.updated_at)
  };
}
function decodeThreadsResponse(value) {
  if (!isObject(value) || !Array.isArray(value.threads)) {
    throw new Error("Invalid thread list response");
  }
  return value.threads.map((thread) => decodeThreadSummary(thread));
}
function decodeCreateThreadResponse(value) {
  if (!isObject(value) || !asString(value.thread_id)) {
    throw new Error("Invalid create thread response");
  }
  return { thread_id: asString(value.thread_id) ?? "" };
}
function decodeEnterChatResponse(value) {
  if (!isObject(value) || !asString(value.session_id) || !asString(value.websocket_url) || typeof value.accepted_protocol_version !== "number") {
    throw new Error("Invalid enter-chat response");
  }
  return {
    session_id: asString(value.session_id) ?? "",
    websocket_url: asString(value.websocket_url) ?? "",
    accepted_protocol_version: value.accepted_protocol_version
  };
}
function decodeCommittedTurn(value) {
  if (!isObject(value) || !asString(value.id) || !asString(value.thread_id) || !asString(value.speaker)) {
    throw new Error("Invalid committed turn");
  }
  const rawToolCalls = Array.isArray(value.tool_calls) ? value.tool_calls : [];
  const toolCalls = rawToolCalls.filter((toolCall) => isObject(toolCall)).map((toolCall) => ({
    id: asString(toolCall.id) ?? "",
    name: asString(toolCall.name) ?? "tool",
    args: isObject(toolCall.args) ? Object.fromEntries(Object.entries(toolCall.args).map(([key, item]) => [key, normalizeJSONValue(item)])) : {},
    result: asString(toolCall.result) ?? "",
    isError: asBoolean(toolCall.is_error)
  }));
  return {
    id: asString(value.id) ?? "",
    thread_id: asString(value.thread_id) ?? "",
    prev_turn_id: asString(value.prev_turn_id),
    speaker: asString(value.speaker) ?? "system",
    message_text: asString(value.message_text) ?? "",
    model_name: asString(value.model_name),
    created_at: asString(value.created_at),
    tool_calls: toolCalls
  };
}
function decodeModelListResponse(value) {
  if (!isObject(value) || !Array.isArray(value.models)) {
    throw new Error("Invalid models response");
  }
  return value.models.filter((item) => isObject(item) && typeof item.name === "string").map((item) => ({
    name: asString(item.name) ?? "",
    provider: asString(item.provider) ?? "unknown",
    is_default: asBoolean(item.is_default)
  }));
}

// src/chat/api.ts
var ChatApiClient = class {
  constructor(settings) {
    this.settings = settings;
  }
  async listThreads() {
    const response = await (0, import_obsidian.requestUrl)({
      url: `${this.settings().serverBaseUrl}/threads`,
      method: "GET"
    });
    return decodeThreadsResponse(response.json);
  }
  async createThread(name) {
    const response = await (0, import_obsidian.requestUrl)({
      url: `${this.settings().serverBaseUrl}/threads`,
      method: "POST",
      contentType: "application/json",
      body: JSON.stringify({ name })
    });
    return decodeCreateThreadResponse(response.json);
  }
  async listModels() {
    const response = await (0, import_obsidian.requestUrl)({
      url: `${this.settings().serverBaseUrl}/models`,
      method: "GET"
    });
    return decodeModelListResponse(response.json);
  }
  async enterThread(threadID, knownTailTurnID) {
    const response = await (0, import_obsidian.requestUrl)({
      url: `${this.settings().serverBaseUrl}/threads/${encodeURIComponent(threadID)}/enter-chat`,
      method: "POST",
      contentType: "application/json",
      body: JSON.stringify({
        protocol_version: CHAT_PROTOCOL_VERSION,
        known_tail_turn_id: knownTailTurnID
      })
    });
    return decodeEnterChatResponse(response.json);
  }
};

// src/chat/toolSummary.ts
var FILE_TOOL_LABELS = {
  read_note: { verb: "Read", fallback: "Read file" },
  read_file: { verb: "Read", fallback: "Read file" },
  write_note: { verb: "Wrote", fallback: "Wrote file" },
  create_file: { verb: "Created", fallback: "Created file" },
  apply_patch: { verb: "Applied patch to", fallback: "Applied patch" },
  patch_note: { verb: "Patched", fallback: "Patched file" }
};
var PATH_KEYS = ["relative_path", "path", "file_path", "filepath", "target_path", "note_path"];
function asString2(value) {
  return typeof value === "string" ? value : null;
}
function basename(path) {
  const trimmed = path.replace(/\\/g, "/").replace(/\/+$/, "");
  const parts = trimmed.split("/").filter(Boolean);
  return parts.length > 0 ? parts[parts.length - 1] : trimmed;
}
function pickPath(args) {
  for (const key of PATH_KEYS) {
    const candidate = asString2(args[key]);
    if (candidate && candidate.trim()) {
      return candidate.trim();
    }
  }
  return null;
}
function formatPath(path) {
  const normalized = path.replace(/\\/g, "/");
  const vaultRelativeMatch = normalized.match(/(?:^|\/)data\/vaults\/[^/]+\/(.+)$/);
  if (vaultRelativeMatch?.[1]) {
    return vaultRelativeMatch[1];
  }
  if (!normalized.startsWith("/") && !/^[A-Za-z]:[\\/]/.test(normalized)) {
    return normalized;
  }
  return basename(normalized);
}
function summarizeToolCall(call) {
  const label = FILE_TOOL_LABELS[call.name];
  const path = pickPath(call.args);
  if (label) {
    if (path) {
      return `${label.verb} ${formatPath(path)}`;
    }
    return label.fallback;
  }
  if (call.name === "list_notes") {
    const dir = asString2(call.args.relative_dir) ?? asString2(call.args.path);
    if (dir && dir.trim()) {
      return `Listed ${formatPath(dir.trim())}`;
    }
    return "Listed directory";
  }
  const fallbackName = call.name.replace(/_/g, " ").trim() || "tool";
  return `${call.isError ? "Tool failed" : "Used"} ${fallbackName}`;
}

// src/chat/store.ts
function buildTranscriptItems(session) {
  const items = [];
  for (const turn of session.committedHistory.turns) {
    if (turn.speaker === "assistant") {
      for (const call of turn.tool_calls) {
        items.push({
          kind: "tool_call",
          id: `tool-${turn.id}-${call.id}`,
          text: summarizeToolCall(call),
          tone: call.isError ? "error" : "normal"
        });
      }
    }
    items.push({
      kind: "committed",
      id: turn.id,
      role: turn.speaker === "assistant" || turn.speaker === "system" ? turn.speaker : "user",
      text: turn.message_text
    });
  }
  for (const pending of session.pendingSends) {
    items.push({
      kind: "pending",
      id: pending.localMessageID,
      role: "user",
      text: pending.text
    });
  }
  for (const stream of Object.values(session.streamingByQueue).sort((a, b) => a.queueID - b.queueID)) {
    items.push({
      kind: "streaming",
      id: `stream-${stream.queueID}`,
      role: "assistant",
      text: stream.text,
      queueID: stream.queueID
    });
  }
  return items;
}
function createChatSession(thread) {
  return {
    thread,
    committedHistory: {
      turns: [],
      lastTurnID: null
    },
    pendingSends: [],
    streamingByQueue: {},
    lastFrameAtMs: null,
    errorMessage: null,
    connectionState: "idle",
    thinkingActive: false,
    statusMessage: null,
    contextBudget: null,
    transcriptItems: []
  };
}
function consumeMatchingPendingSend(session, turn) {
  if (turn.speaker !== "user") {
    return;
  }
  const index = session.pendingSends.findIndex((pending) => {
    if (pending.text !== turn.message_text) {
      return false;
    }
    return pending.responseToTurnID === turn.prev_turn_id || pending.responseToTurnID === session.committedHistory.lastTurnID;
  });
  if (index >= 0) {
    session.pendingSends.splice(index, 1);
  }
}
function getLastCommittedTurnID(session) {
  return session.committedHistory.lastTurnID;
}
function clearCommittedHistory(session) {
  session.committedHistory = {
    turns: [],
    lastTurnID: null
  };
  return rebuildTranscript(session);
}
function rebuildTranscript(session) {
  session.transcriptItems = buildTranscriptItems(session);
  return session;
}
function dropUncommittedArtifacts(session) {
  session.pendingSends = [];
  session.streamingByQueue = {};
  session.thinkingActive = false;
  session.statusMessage = null;
  return rebuildTranscript(session);
}
function dropQueueArtifacts(session, queueID) {
  session.pendingSends = session.pendingSends.filter((pending) => pending.queueID !== queueID);
  delete session.streamingByQueue[queueID];
  if (Object.keys(session.streamingByQueue).length === 0) {
    session.thinkingActive = false;
  }
  if (Object.keys(session.streamingByQueue).length === 0 && session.pendingSends.length === 0) {
    session.statusMessage = null;
  }
  return rebuildTranscript(session);
}
function applyCommittedTurn(session, turn) {
  if (session.committedHistory.turns.some((existing) => existing.id === turn.id)) {
    return session;
  }
  session.committedHistory.turns.push(turn);
  session.committedHistory.lastTurnID = turn.id;
  consumeMatchingPendingSend(session, turn);
  return rebuildTranscript(session);
}
var ChatStore = class {
  listeners = /* @__PURE__ */ new Set();
  sessions = /* @__PURE__ */ new Map();
  state = {
    screen: "threads",
    threadList: {
      loading: false,
      creating: false,
      errorMessage: null,
      threads: []
    },
    activeThreadId: null,
    activeSession: null
  };
  subscribe(listener) {
    this.listeners.add(listener);
    return () => {
      this.listeners.delete(listener);
    };
  }
  getSnapshot() {
    return this.state;
  }
  getActiveSession() {
    return this.state.activeSession;
  }
  getSession(threadID) {
    return this.sessions.get(threadID) ?? null;
  }
  setThreadListLoading(loading) {
    this.state = {
      ...this.state,
      threadList: {
        ...this.state.threadList,
        loading,
        errorMessage: loading ? null : this.state.threadList.errorMessage
      }
    };
    this.emit();
  }
  setThreadListCreating(creating) {
    this.state = {
      ...this.state,
      threadList: {
        ...this.state.threadList,
        creating
      }
    };
    this.emit();
  }
  setThreadListError(message) {
    this.state = {
      ...this.state,
      threadList: {
        ...this.state.threadList,
        loading: false,
        errorMessage: message
      }
    };
    this.emit();
  }
  setThreads(threads) {
    const existingSessions = /* @__PURE__ */ new Map();
    for (const thread of threads) {
      const current = this.sessions.get(thread.thread_id);
      existingSessions.set(thread.thread_id, rebuildTranscript({ ...current ?? createChatSession(thread), thread }));
    }
    for (const [threadID, session] of this.sessions.entries()) {
      if (!existingSessions.has(threadID)) {
        existingSessions.set(threadID, session);
      }
    }
    this.sessions.clear();
    for (const [threadID, session] of existingSessions.entries()) {
      this.sessions.set(threadID, session);
    }
    this.state = {
      ...this.state,
      threadList: {
        ...this.state.threadList,
        loading: false,
        errorMessage: null,
        threads
      },
      activeSession: this.state.activeThreadId ? this.sessions.get(this.state.activeThreadId) ?? null : null
    };
    this.emit();
  }
  showThreadList() {
    this.state = {
      ...this.state,
      screen: "threads",
      activeThreadId: null,
      activeSession: null
    };
    this.emit();
  }
  openConversation(thread) {
    const current = this.sessions.get(thread.thread_id);
    const session = rebuildTranscript({ ...current ?? createChatSession(thread), thread });
    this.sessions.set(thread.thread_id, session);
    this.state = {
      ...this.state,
      screen: "conversation",
      activeThreadId: thread.thread_id,
      activeSession: session
    };
    this.emit();
    return session;
  }
  updateSession(threadID, updater) {
    const session = this.sessions.get(threadID);
    if (!session) {
      throw new Error(`Unknown chat session for thread ${threadID}`);
    }
    updater(session);
    rebuildTranscript(session);
    if (this.state.activeThreadId === threadID) {
      this.state = {
        ...this.state,
        activeSession: session
      };
    }
    this.emit();
    return session;
  }
  replaceSession(threadID, session) {
    this.sessions.set(threadID, rebuildTranscript(session));
    if (this.state.activeThreadId === threadID) {
      this.state = {
        ...this.state,
        activeSession: this.sessions.get(threadID) ?? null
      };
    }
    this.emit();
  }
  findThread(threadID) {
    const inList = this.state.threadList.threads.find((thread) => thread.thread_id === threadID);
    if (inList) {
      return inList;
    }
    return this.sessions.get(threadID)?.thread ?? null;
  }
  emit() {
    for (const listener of this.listeners) {
      listener();
    }
  }
};

// src/chat/transport.ts
function isObject2(value) {
  return typeof value === "object" && value !== null;
}
function asString3(value) {
  return typeof value === "string" ? value : null;
}
function asNumber(value) {
  return typeof value === "number" ? value : null;
}
function toJSONValue(value) {
  if (value === null || typeof value === "string" || typeof value === "number" || typeof value === "boolean") {
    return value;
  }
  if (Array.isArray(value)) {
    return value.map((item) => toJSONValue(item) ?? null);
  }
  if (isObject2(value)) {
    return Object.fromEntries(Object.entries(value).map(([key, item]) => [key, toJSONValue(item) ?? null]));
  }
  return null;
}
function decodeChatTransportEvent(payload) {
  if (!isObject2(payload) || !asString3(payload.type)) {
    return null;
  }
  const type = asString3(payload.type) ?? "";
  switch (type) {
    case "handshake_snapshot_begin":
      return { type: "handshake_begin", threadID: asString3(payload.thread_id) };
    case "handshake_ready":
      return { type: "handshake_ready" };
    case "message_durable_ack":
      return {
        type: "durable_ack",
        queueID: asNumber(payload.queue_id) ?? -1,
        clientMessageID: asString3(payload.client_message_id)
      };
    case "assistant_token":
      return {
        type: "assistant_token",
        queueID: asNumber(payload.queue_id) ?? -1,
        chunk: asString3(payload.chunk) ?? ""
      };
    case "committed_turn":
      return { type: "committed_turn", turn: decodeCommittedTurn(payload.turn) };
    case "turn_finalized":
      return {
        type: "turn_finalized",
        queueID: asNumber(payload.queue_id) ?? -1,
        turnID: asString3(payload.turn_id)
      };
    case "execution_conflict":
      return {
        type: "execution_conflict",
        queueID: asNumber(payload.queue_id) ?? -1,
        expectedTailTurnID: asString3(payload.expected_tail_turn_id),
        actualTailTurnID: asString3(payload.actual_tail_turn_id)
      };
    case "context_budget":
      return {
        type: "context_budget",
        context: {
          contextSize: asNumber(payload.context_size) ?? 0,
          maxContextSize: asNumber(payload.max_context_size) ?? 0
        }
      };
    case "turn_event":
      return {
        type: "turn_event",
        queueID: asNumber(payload.queue_id) ?? -1,
        event: asString3(payload.event) ?? "unknown",
        trace: asString3(payload.trace),
        traceKind: asString3(payload.trace_kind),
        payload: toJSONValue(payload.payload)
      };
    case "heartbeat":
      return { type: "heartbeat", intervalSeconds: asNumber(payload.interval_seconds) };
    case "error":
      return {
        type: "error",
        message: asString3(payload.message) ?? "Unknown chat transport error",
        queueID: asNumber(payload.queue_id),
        fatal: payload.fatal === true
      };
    default:
      return null;
  }
}
var ChatTransportClient = class {
  constructor(settings) {
    this.settings = settings;
  }
  socket = null;
  intentionalClose = false;
  async connect(websocketPath, onEvent) {
    this.disconnect();
    this.intentionalClose = false;
    await new Promise((resolve, reject) => {
      const socket = new WebSocket(this.websocketURL(websocketPath));
      this.socket = socket;
      socket.addEventListener("open", () => resolve(), { once: true });
      socket.addEventListener("error", () => reject(new Error("Chat websocket failed to connect")), { once: true });
      socket.addEventListener("message", (event) => {
        try {
          const raw = JSON.parse(String(event.data));
          const decoded = decodeChatTransportEvent(raw);
          if (decoded) {
            onEvent(decoded);
          }
        } catch (error) {
          onEvent({
            type: "error",
            message: error instanceof Error ? error.message : "Malformed websocket payload",
            queueID: null,
            fatal: true
          });
        }
      });
      socket.addEventListener("close", (event) => {
        this.socket = null;
        if (this.intentionalClose) {
          return;
        }
        const closedEvent = {
          type: "closed",
          code: event.code,
          reason: event.reason || null
        };
        onEvent(closedEvent);
      });
    });
  }
  disconnect() {
    this.intentionalClose = true;
    this.socket?.close();
    this.socket = null;
  }
  async submitMessage(args) {
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
        client_message_id: args.clientMessageID
      })
    );
  }
  websocketURL(path) {
    if (/^wss?:\/\//.test(path)) {
      return path;
    }
    const baseUrl = new URL(this.settings().serverBaseUrl);
    baseUrl.protocol = baseUrl.protocol === "https:" ? "wss:" : "ws:";
    baseUrl.pathname = path;
    baseUrl.search = "";
    return baseUrl.toString();
  }
};

// src/chat/service.ts
var ChatService = class {
  constructor(options) {
    this.options = options;
    this.api = new ChatApiClient(options.settings);
    this.transport = new ChatTransportClient(options.settings);
    this.getNow = options.getNow ?? (() => Date.now());
  }
  api;
  transport;
  store = new ChatStore();
  getNow;
  activeThreadID = null;
  connectAttempt = 0;
  watchdogHandle = null;
  reconnectHandle = null;
  thinkingQuietHandle = null;
  subscribe(listener) {
    return this.store.subscribe(listener);
  }
  getSnapshot() {
    return this.store.getSnapshot();
  }
  async activateView() {
    await this.showThreadList();
  }
  async deactivateView() {
    this.clearTimers();
    this.transport.disconnect();
    this.activeThreadID = null;
    this.store.showThreadList();
  }
  openSettings() {
    this.options.openSettings();
  }
  async showThreadList() {
    this.clearTimers();
    this.transport.disconnect();
    this.activeThreadID = null;
    this.store.showThreadList();
    await this.refreshThreads();
  }
  async refreshThreads() {
    this.store.setThreadListLoading(true);
    try {
      const threads = await this.api.listThreads();
      this.store.setThreads(threads);
    } catch (error) {
      this.store.setThreadListError(error instanceof Error ? error.message : String(error));
    }
  }
  async createThread(name = "New thread") {
    this.store.setThreadListCreating(true);
    try {
      const trimmedName = name.trim() || "New thread";
      const created = await this.api.createThread(trimmedName);
      await this.refreshThreads();
      const thread = this.store.findThread(created.thread_id) ?? {
        thread_id: created.thread_id,
        name: trimmedName,
        prev_thread_id: null,
        start_turn_id: null,
        is_archived: false,
        tail_turn_id: null,
        created_at: null,
        updated_at: null
      };
      await this.openThread(thread.thread_id, thread);
    } catch (error) {
      this.store.setThreadListError(error instanceof Error ? error.message : String(error));
    } finally {
      this.store.setThreadListCreating(false);
    }
  }
  async openThread(threadID, providedThread) {
    const thread = providedThread ?? this.store.findThread(threadID);
    if (!thread) {
      throw new Error(`Unknown thread ${threadID}`);
    }
    this.clearTimers();
    this.transport.disconnect();
    const session = this.store.openConversation(thread);
    session.connectionState = "connecting";
    session.errorMessage = null;
    session.statusMessage = "Connecting\u2026";
    this.store.replaceSession(threadID, session);
    this.activeThreadID = threadID;
    const attempt = ++this.connectAttempt;
    try {
      const enter = await this.api.enterThread(threadID, getLastCommittedTurnID(session));
      if (attempt !== this.connectAttempt || this.activeThreadID !== threadID) {
        return;
      }
      await this.transport.connect(enter.websocket_url, (event) => {
        this.handleTransportEvent(threadID, event);
      });
      if (attempt !== this.connectAttempt || this.activeThreadID !== threadID) {
        this.transport.disconnect();
        return;
      }
      this.startWatchdog();
    } catch (error) {
      this.store.updateSession(threadID, (current) => {
        current.connectionState = "error";
        current.errorMessage = error instanceof Error ? error.message : String(error);
        current.statusMessage = null;
      });
      this.scheduleReconnect(threadID);
    }
  }
  async sendMessage(text) {
    const threadID = this.activeThreadID;
    if (!threadID) {
      return false;
    }
    const trimmed = text.trim();
    if (!trimmed) {
      return false;
    }
    const session = this.store.getSession(threadID);
    if (!session || session.connectionState !== "live") {
      return false;
    }
    const clientMessageID = globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random()}`;
    const localMessageID = `local-${clientMessageID}`;
    const modelName = this.options.settings().chatDefaultModel;
    const inResponseToTurnID = getLastCommittedTurnID(session);
    this.store.updateSession(threadID, (session2) => {
      session2.pendingSends.push({
        clientMessageID,
        text: trimmed,
        responseToTurnID: inResponseToTurnID,
        localMessageID,
        queueID: null
      });
      session2.errorMessage = null;
      session2.statusMessage = "Sending\u2026";
    });
    try {
      await this.transport.submitMessage({
        threadID,
        text: trimmed,
        modelName,
        inResponseToTurnID,
        clientMessageID
      });
    } catch (error) {
      this.store.updateSession(threadID, (session2) => {
        session2.errorMessage = error instanceof Error ? error.message : String(error);
        session2.statusMessage = null;
        dropUncommittedArtifacts(session2);
      });
      return false;
    }
    return true;
  }
  handleTransportEvent(threadID, event) {
    if (threadID !== this.activeThreadID) {
      return;
    }
    const now = this.getNow();
    if (event.type === "heartbeat") {
      const session = this.store.getSession(threadID);
      if (session) {
        session.lastFrameAtMs = now;
      }
      return;
    }
    if (event.type === "closed") {
      this.store.updateSession(threadID, (session) => {
        session.connectionState = "error";
        session.statusMessage = "Disconnected";
        session.errorMessage = event.reason ?? `Connection closed (${event.code})`;
      });
      this.scheduleReconnect(threadID);
      return;
    }
    this.store.updateSession(threadID, (session) => {
      session.lastFrameAtMs = now;
      switch (event.type) {
        case "handshake_begin":
          session.connectionState = "replaying";
          session.errorMessage = null;
          session.statusMessage = "Loading chat history\u2026";
          clearCommittedHistory(session);
          dropUncommittedArtifacts(session);
          break;
        case "handshake_ready":
          session.connectionState = "live";
          session.errorMessage = null;
          session.statusMessage = null;
          break;
        case "durable_ack":
          if (event.clientMessageID) {
            const pending = session.pendingSends.find((item) => item.clientMessageID === event.clientMessageID);
            if (pending) {
              pending.queueID = event.queueID;
            }
          }
          session.statusMessage = "Waiting for response\u2026";
          break;
        case "assistant_token":
          session.streamingByQueue[event.queueID] = {
            queueID: event.queueID,
            text: `${session.streamingByQueue[event.queueID]?.text ?? ""}${event.chunk}`
          };
          session.statusMessage = "Assistant is responding\u2026";
          break;
        case "committed_turn":
          applyCommittedTurn(session, event.turn);
          session.statusMessage = null;
          break;
        case "turn_finalized":
          delete session.streamingByQueue[event.queueID];
          session.statusMessage = null;
          session.thinkingActive = false;
          break;
        case "execution_conflict":
          session.connectionState = "connecting";
          session.errorMessage = "Chat changed elsewhere. Resyncing\u2026";
          session.statusMessage = "Resyncing\u2026";
          dropUncommittedArtifacts(session);
          this.transport.disconnect();
          this.stopWatchdog();
          this.scheduleReconnect(threadID);
          return;
        case "context_budget":
          session.contextBudget = event.context;
          break;
        case "turn_event":
          if (event.event === "execution_started") {
            session.thinkingActive = true;
            session.statusMessage = "Thinking\u2026";
          } else if (event.event === "thinking_trace") {
            session.thinkingActive = true;
            session.statusMessage = "Thinking\u2026";
          } else if (event.event === "context_compaction") {
            session.statusMessage = "Condensing context\u2026";
          }
          this.bumpThinkingQuietTimer(threadID);
          break;
        case "error":
          session.errorMessage = event.message;
          session.statusMessage = null;
          if (event.queueID !== null && event.queueID >= 0) {
            if (event.fatal) {
              dropQueueArtifacts(session, event.queueID);
            } else {
              delete session.streamingByQueue[event.queueID];
              if (Object.keys(session.streamingByQueue).length === 0) {
                session.thinkingActive = false;
              }
            }
            session.statusMessage = this.deriveInFlightStatus(session);
            break;
          }
          session.connectionState = "error";
          session.thinkingActive = false;
          if (event.fatal) {
            dropUncommittedArtifacts(session);
          }
          break;
      }
    });
  }
  deriveInFlightStatus(session) {
    if (Object.keys(session.streamingByQueue).length > 0) {
      return "Assistant is responding\u2026";
    }
    if (session.pendingSends.some((pending) => pending.queueID !== null)) {
      return "Waiting for response\u2026";
    }
    if (session.pendingSends.length > 0) {
      return "Sending\u2026";
    }
    return null;
  }
  scheduleReconnect(threadID) {
    if (this.reconnectHandle !== null || threadID !== this.activeThreadID) {
      return;
    }
    this.reconnectHandle = window.setTimeout(() => {
      this.reconnectHandle = null;
      const thread = this.store.findThread(threadID);
      if (!thread || this.activeThreadID !== threadID) {
        return;
      }
      void this.openThread(threadID, thread);
    }, this.options.settings().chatReconnectDelayMs);
  }
  startWatchdog() {
    this.stopWatchdog();
    this.watchdogHandle = window.setInterval(() => {
      const threadID = this.activeThreadID;
      const session = threadID ? this.store.getSession(threadID) : null;
      if (!threadID || !session || session.connectionState === "connecting") {
        return;
      }
      const lastFrameAtMs = session.lastFrameAtMs ?? 0;
      if (lastFrameAtMs === 0) {
        return;
      }
      if (this.getNow() - lastFrameAtMs < this.options.settings().chatWatchdogMs) {
        return;
      }
      this.store.updateSession(threadID, (current) => {
        current.connectionState = "connecting";
        current.errorMessage = "Connection stalled. Reconnecting\u2026";
        current.statusMessage = "Reconnecting\u2026";
        dropUncommittedArtifacts(current);
      });
      void this.openThread(threadID, session.thread);
    }, 5e3);
  }
  stopWatchdog() {
    if (this.watchdogHandle !== null) {
      window.clearInterval(this.watchdogHandle);
      this.watchdogHandle = null;
    }
  }
  bumpThinkingQuietTimer(threadID) {
    if (this.thinkingQuietHandle !== null) {
      window.clearTimeout(this.thinkingQuietHandle);
    }
    this.thinkingQuietHandle = window.setTimeout(() => {
      this.thinkingQuietHandle = null;
      const session = this.store.getSession(threadID);
      if (!session) {
        return;
      }
      this.store.updateSession(threadID, (current) => {
        if (Object.keys(current.streamingByQueue).length === 0) {
          current.thinkingActive = false;
          if (current.statusMessage === "Thinking\u2026") {
            current.statusMessage = null;
          }
        }
      });
    }, 2500);
  }
  clearTimers() {
    this.stopWatchdog();
    if (this.reconnectHandle !== null) {
      window.clearTimeout(this.reconnectHandle);
      this.reconnectHandle = null;
    }
    if (this.thinkingQuietHandle !== null) {
      window.clearTimeout(this.thinkingQuietHandle);
      this.thinkingQuietHandle = null;
    }
  }
};

// src/chat/view.ts
var import_obsidian5 = require("obsidian");

// src/chat/components/threadList.ts
var import_obsidian3 = require("obsidian");

// src/chat/modals.ts
var import_obsidian2 = require("obsidian");
var DEFAULT_NEW_THREAD_NAME = "New thread";
var NewThreadModal = class extends import_obsidian2.Modal {
  constructor(app, onSubmit) {
    super(app);
    this.onSubmit = onSubmit;
  }
  name = DEFAULT_NEW_THREAD_NAME;
  onOpen() {
    const { contentEl } = this;
    contentEl.empty();
    this.titleEl.setText("New thread");
    let inputEl = null;
    new import_obsidian2.Setting(contentEl).setName("Thread name").addText((text) => {
      inputEl = text.inputEl;
      text.setValue(this.name).onChange((value) => {
        this.name = value;
      });
      text.inputEl.addEventListener("keydown", (event) => {
        if (event.key === "Enter") {
          event.preventDefault();
          this.onSubmit(this.name);
          this.close();
        }
      });
    });
    const actions = contentEl.createDiv({ cls: "sheaf-modal-actions" });
    actions.createEl("button", { text: "Cancel" }).addEventListener("click", () => this.close());
    actions.createEl("button", { text: "Create", cls: "mod-cta" }).addEventListener("click", () => {
      this.onSubmit(this.name);
      this.close();
    });
    window.setTimeout(() => inputEl?.focus(), 0);
  }
};

// src/chat/components/threadList.ts
function formatWhen(value) {
  if (!value) {
    return null;
  }
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) {
    return value;
  }
  return parsed.toLocaleString();
}
var ThreadListComponent = class {
  container = null;
  mount(parent) {
    parent.empty();
    parent.addClass("sheaf-root");
    this.container = parent;
  }
  update(state, service, app) {
    if (!this.container) {
      return;
    }
    this.container.empty();
    const header = this.container.createDiv({ cls: "sheaf-header" });
    header.createDiv({ text: "Sheaf Chat", cls: "sheaf-header-title" });
    const actions = header.createDiv({ cls: "sheaf-header-actions" });
    const refreshBtn = actions.createEl("button", { cls: "sheaf-icon-btn" });
    (0, import_obsidian3.setIcon)(refreshBtn, "refresh-cw");
    refreshBtn.setAttribute("aria-label", "Refresh threads");
    refreshBtn.addEventListener("click", () => {
      void service.refreshThreads();
    });
    const settingsBtn = actions.createEl("button", { cls: "sheaf-icon-btn" });
    (0, import_obsidian3.setIcon)(settingsBtn, "settings");
    settingsBtn.setAttribute("aria-label", "Open settings");
    settingsBtn.addEventListener("click", () => {
      service.openSettings();
    });
    const newBtn = this.container.createEl("button", { cls: "sheaf-new-thread-btn" });
    const plusIcon = newBtn.createSpan();
    (0, import_obsidian3.setIcon)(plusIcon, "plus");
    newBtn.createSpan({ text: "New thread" });
    newBtn.disabled = state.threadList.creating;
    newBtn.setAttribute("aria-label", "Create a new thread");
    newBtn.addEventListener("click", () => {
      new NewThreadModal(app, (name) => {
        void service.createThread(name).catch((error) => {
          new import_obsidian3.Notice(error instanceof Error ? error.message : String(error));
        });
      }).open();
    });
    if (state.threadList.loading) {
      this.container.createDiv({ text: "Loading threads\u2026", cls: "sheaf-empty-state" });
      return;
    }
    if (state.threadList.errorMessage) {
      this.container.createDiv({ text: state.threadList.errorMessage, cls: "sheaf-error-text" });
    }
    if (state.threadList.threads.length === 0) {
      this.container.createDiv({
        text: "No threads yet. Create one to start chatting.",
        cls: "sheaf-empty-state"
      });
      return;
    }
    const list = this.container.createDiv({ cls: "sheaf-thread-list" });
    list.setAttribute("role", "list");
    list.setAttribute("aria-label", "Chat threads");
    for (const thread of state.threadList.threads) {
      const card = list.createEl("button", { cls: "sheaf-thread-card" });
      card.setAttribute("role", "listitem");
      card.createSpan({ text: thread.name || thread.thread_id, cls: "sheaf-thread-card-title" });
      const updated = formatWhen(thread.updated_at);
      if (updated) {
        card.createSpan({ text: `Updated ${updated}`, cls: "sheaf-thread-card-meta" });
      }
      card.addEventListener("click", () => {
        void service.openThread(thread.thread_id).catch((error) => {
          new import_obsidian3.Notice(error instanceof Error ? error.message : String(error));
        });
      });
    }
  }
  destroy() {
    this.container = null;
  }
};

// src/chat/components/conversation.ts
var import_obsidian4 = require("obsidian");

// src/chat/components/messageRenderer.ts
function itemSignature(item) {
  switch (item.kind) {
    case "tool_call":
      return `${item.kind}:${item.text}:${item.tone}`;
    case "streaming":
      return `${item.kind}:${item.text}:${item.queueID}`;
    default:
      return `${item.kind}:${item.role}:${item.text}`;
  }
}
function renderTranscriptItem(item) {
  const element = document.createElement("div");
  element.dataset.signature = itemSignature(item);
  element.setAttribute("role", "listitem");
  if (item.kind === "tool_call") {
    element.addClasses(["sheaf-message", "sheaf-message--tool"]);
    if (item.tone === "error") {
      element.addClass("sheaf-message--tool-error");
    }
    element.setText(item.text);
    return element;
  }
  element.addClass("sheaf-message");
  if (item.role === "user") {
    element.addClass("sheaf-message--user");
  } else if (item.role === "assistant") {
    element.addClass("sheaf-message--assistant");
  } else {
    element.addClass("sheaf-message--system");
  }
  if (item.kind === "pending") {
    element.addClass("sheaf-message--pending");
  }
  if (item.kind === "streaming") {
    element.addClass("sheaf-message--streaming");
  }
  element.setText(item.text);
  return element;
}
function updateTranscriptItem(element, item) {
  const newSig = itemSignature(item);
  if (element.dataset.signature === newSig) {
    return;
  }
  if (item.kind === "streaming" && element.hasClass("sheaf-message--streaming")) {
    element.textContent = item.text;
    element.dataset.signature = newSig;
    return;
  }
  const replacement = renderTranscriptItem(item);
  element.className = replacement.className;
  element.textContent = replacement.textContent;
  element.dataset.signature = replacement.dataset.signature;
}

// src/chat/components/conversation.ts
function formatWhen2(value) {
  if (!value) {
    return null;
  }
  const parsed = new Date(value);
  if (Number.isNaN(parsed.getTime())) {
    return value;
  }
  return parsed.toLocaleString();
}
var ConversationComponent = class {
  container = null;
  titleEl = null;
  subtitleEl = null;
  transcriptEl = null;
  statusEl = null;
  composerEl = null;
  sendBtn = null;
  composerValue = "";
  transcriptRowMap = /* @__PURE__ */ new Map();
  lastTranscriptSnapshot = /* @__PURE__ */ new Map();
  lastStatusText = "";
  mountedThreadID = null;
  mount(parent, service) {
    parent.empty();
    parent.addClass("sheaf-root");
    this.container = parent;
    const conversation = this.container.createDiv({ cls: "sheaf-conversation" });
    const header = conversation.createDiv({ cls: "sheaf-conv-header" });
    const backBtn = header.createEl("button", { cls: "sheaf-icon-btn" });
    (0, import_obsidian4.setIcon)(backBtn, "arrow-left");
    backBtn.setAttribute("aria-label", "Back to threads");
    backBtn.addEventListener("click", () => {
      void service.showThreadList();
    });
    const info = header.createDiv({ cls: "sheaf-conv-header-info" });
    this.titleEl = info.createDiv({ cls: "sheaf-conv-title" });
    this.subtitleEl = info.createDiv({ cls: "sheaf-conv-subtitle" });
    const settingsBtn = header.createEl("button", { cls: "sheaf-icon-btn" });
    (0, import_obsidian4.setIcon)(settingsBtn, "settings");
    settingsBtn.setAttribute("aria-label", "Open settings");
    settingsBtn.addEventListener("click", () => {
      service.openSettings();
    });
    this.transcriptEl = conversation.createDiv({ cls: "sheaf-transcript" });
    this.transcriptEl.setAttribute("role", "log");
    this.transcriptEl.setAttribute("aria-label", "Chat messages");
    this.statusEl = conversation.createDiv({ cls: "sheaf-status" });
    this.statusEl.setAttribute("aria-live", "polite");
    const composer = conversation.createDiv({ cls: "sheaf-composer" });
    this.composerEl = composer.createEl("textarea", {
      cls: "sheaf-composer-textarea",
      attr: { placeholder: "Message Sheaf\u2026", rows: "1" }
    });
    this.composerEl.setAttribute("aria-label", "Message input");
    this.composerEl.addEventListener("input", () => {
      this.composerValue = this.composerEl?.value ?? "";
      this.autosizeComposer();
    });
    this.composerEl.addEventListener("keydown", (event) => {
      if (event.key === "Enter" && !event.shiftKey) {
        event.preventDefault();
        void this.submitComposer(service);
      }
    });
    const row = composer.createDiv({ cls: "sheaf-composer-row" });
    row.createDiv({ text: "Enter sends, Shift+Enter new line", cls: "sheaf-composer-hint" });
    this.sendBtn = row.createEl("button", { cls: "sheaf-send-btn" });
    (0, import_obsidian4.setIcon)(this.sendBtn, "send");
    this.sendBtn.setAttribute("aria-label", "Send message");
    this.sendBtn.addEventListener("click", () => {
      void this.submitComposer(service);
    });
  }
  update(session) {
    if (!session) {
      this.transcriptEl?.empty();
      this.titleEl?.setText("");
      this.subtitleEl?.setText("");
      return;
    }
    if (this.mountedThreadID !== session.thread.thread_id) {
      this.resetTranscript();
      this.mountedThreadID = session.thread.thread_id;
    }
    this.titleEl?.setText(session.thread.name || session.thread.thread_id);
    const updated = formatWhen2(session.thread.updated_at);
    this.subtitleEl?.setText(updated ? `Updated ${updated}` : "");
    this.syncTranscript(session);
    this.syncStatus(session);
    this.syncComposer(session);
  }
  scrollToBottomIfNeeded() {
    if (!this.transcriptEl) {
      return;
    }
    if (this.isNearBottom(this.transcriptEl)) {
      this.transcriptEl.scrollTop = this.transcriptEl.scrollHeight;
    }
  }
  destroy() {
    this.container = null;
    this.titleEl = null;
    this.subtitleEl = null;
    this.transcriptEl = null;
    this.statusEl = null;
    this.composerEl = null;
    this.sendBtn = null;
    this.transcriptRowMap.clear();
    this.lastTranscriptSnapshot.clear();
    this.lastStatusText = "";
    this.mountedThreadID = null;
    this.composerValue = "";
  }
  syncTranscript(session) {
    if (!this.transcriptEl) {
      return;
    }
    const visibleSnapshot = /* @__PURE__ */ new Map();
    for (const item of session.transcriptItems) {
      visibleSnapshot.set(item.id, itemSignature(item));
    }
    let changed = visibleSnapshot.size !== this.lastTranscriptSnapshot.size;
    if (!changed) {
      for (const [id, sig] of visibleSnapshot.entries()) {
        if (this.lastTranscriptSnapshot.get(id) !== sig) {
          changed = true;
          break;
        }
      }
    }
    if (!changed) {
      return;
    }
    const wasNearBottom = this.isNearBottom(this.transcriptEl);
    const nextIds = new Set(session.transcriptItems.map((item) => item.id));
    for (const [id, node] of this.transcriptRowMap.entries()) {
      if (!nextIds.has(id)) {
        node.remove();
        this.transcriptRowMap.delete(id);
      }
    }
    for (const item of session.transcriptItems) {
      const existing = this.transcriptRowMap.get(item.id);
      if (existing) {
        updateTranscriptItem(existing, item);
        this.transcriptEl.appendChild(existing);
        continue;
      }
      const node = renderTranscriptItem(item);
      this.transcriptRowMap.set(item.id, node);
      this.transcriptEl.appendChild(node);
    }
    this.lastTranscriptSnapshot = visibleSnapshot;
    if (wasNearBottom) {
      this.transcriptEl.scrollTop = this.transcriptEl.scrollHeight;
    }
  }
  syncStatus(session) {
    if (!this.statusEl) {
      return;
    }
    const text = session.errorMessage ?? session.statusMessage ?? (session.thinkingActive ? "Thinking\u2026" : session.connectionState === "connecting" ? "Connecting\u2026" : "");
    if (text === this.lastStatusText) {
      return;
    }
    this.lastStatusText = text;
    this.statusEl.setText(text);
    this.statusEl.className = session.errorMessage ? "sheaf-status sheaf-status--error" : "sheaf-status";
  }
  syncComposer(session) {
    if (!this.composerEl) {
      return;
    }
    const canSend = session.connectionState === "live";
    this.composerEl.disabled = !canSend;
    this.composerEl.placeholder = canSend ? "Message Sheaf\u2026" : "Loading chat history\u2026";
    if (this.sendBtn) {
      this.sendBtn.disabled = !canSend;
    }
    if (this.composerEl.value !== this.composerValue && document.activeElement !== this.composerEl) {
      this.composerEl.value = this.composerValue;
    }
    this.autosizeComposer();
  }
  async submitComposer(service) {
    const value = this.composerEl?.value ?? "";
    const sent = await service.sendMessage(value);
    if (sent) {
      this.clearComposer();
    }
  }
  clearComposer() {
    this.composerValue = "";
    if (!this.composerEl) {
      return;
    }
    this.composerEl.value = "";
    this.composerEl.blur();
    this.autosizeComposer();
  }
  autosizeComposer() {
    if (!this.composerEl) {
      return;
    }
    const baseHeight = 44;
    const maxHeight = Math.max(88, Math.floor(window.innerHeight * 0.35));
    this.composerEl.style.height = `${baseHeight}px`;
    const scrollH = this.composerEl.scrollHeight;
    const nextHeight = Math.min(Math.max(scrollH, baseHeight), maxHeight);
    this.composerEl.style.height = `${nextHeight}px`;
    this.composerEl.style.overflowY = scrollH > maxHeight ? "auto" : "hidden";
  }
  resetTranscript() {
    this.transcriptEl?.empty();
    this.transcriptRowMap.clear();
    this.lastTranscriptSnapshot.clear();
    this.lastStatusText = "";
  }
  isNearBottom(el) {
    return el.scrollHeight - el.scrollTop - el.clientHeight < 24;
  }
};

// src/chat/view.ts
var SHEAF_CHAT_VIEW_TYPE = "sheaf-chat-view";
var SheafChatView = class extends import_obsidian5.ItemView {
  constructor(leaf, chatService) {
    super(leaf);
    this.chatService = chatService;
  }
  unsubscribe = null;
  resizeObserver = null;
  mountedScreen = null;
  threadList = new ThreadListComponent();
  conversation = new ConversationComponent();
  getViewType() {
    return SHEAF_CHAT_VIEW_TYPE;
  }
  getDisplayText() {
    return "Sheaf Chat";
  }
  async onOpen() {
    this.contentEl.style.display = "flex";
    this.contentEl.style.flexDirection = "column";
    this.observeParentSize();
    this.unsubscribe = this.chatService.subscribe(() => {
      this.render(this.chatService.getSnapshot());
    });
    await this.chatService.activateView();
    this.render(this.chatService.getSnapshot());
  }
  async onClose() {
    this.unsubscribe?.();
    this.unsubscribe = null;
    this.resizeObserver?.disconnect();
    this.resizeObserver = null;
    this.contentEl.style.display = "";
    this.contentEl.style.flexDirection = "";
    this.contentEl.style.height = "";
    this.threadList.destroy();
    this.conversation.destroy();
    this.mountedScreen = null;
    await this.chatService.deactivateView();
  }
  render(state) {
    if (state.screen === "threads") {
      if (this.mountedScreen !== "threads") {
        this.conversation.destroy();
        this.contentEl.empty();
        this.contentEl.removeClass("sheaf-root");
        this.threadList.mount(this.contentEl);
        this.mountedScreen = "threads";
      }
      this.threadList.update(state, this.chatService, this.app);
      return;
    }
    if (this.mountedScreen !== "conversation") {
      this.threadList.destroy();
      this.contentEl.empty();
      this.contentEl.removeClass("sheaf-root");
      this.conversation.mount(this.contentEl, this.chatService);
      this.mountedScreen = "conversation";
    }
    this.conversation.update(state.activeSession);
  }
  // Obsidian's parent container doesn't give contentEl a CSS-definite
  // height (it's sized by flex layout, not an explicit height property).
  // We observe the parent's actual size and set an explicit pixel height
  // on contentEl so inner flex children can resolve flex-grow.
  //
  observeParentSize() {
    const parent = this.contentEl.parentElement;
    if (!parent) {
      return;
    }
    let lastHeight = "";
    const sync = () => {
      const parentRect = parent.getBoundingClientRect();
      const elRect = this.contentEl.getBoundingClientRect();
      const offset = elRect.top - parentRect.top;
      const available = parentRect.height - offset;
      const h = `${Math.max(200, Math.round(available))}px`;
      if (h !== lastHeight) {
        this.contentEl.style.height = h;
        lastHeight = h;
      }
    };
    this.resizeObserver = new ResizeObserver(sync);
    this.resizeObserver.observe(parent);
    sync();
  }
};

// src/editProtection.ts
var import_state = require("@codemirror/state");
var REPLICA_SYSTEM_WRITE = import_state.Annotation.define();
function createReplicaEditBlocker(isReplicaProtectedFile) {
  return import_state.EditorState.transactionFilter.of((transaction) => {
    if (!transaction.docChanged) {
      return transaction;
    }
    if (transaction.annotation(REPLICA_SYSTEM_WRITE) === true) {
      return transaction;
    }
    if (!isReplicaProtectedFile()) {
      return transaction;
    }
    return [];
  });
}

// src/checksum.ts
async function sha256Text(text) {
  if (!globalThis.crypto?.subtle) {
    throw new Error("Web Crypto is unavailable; cannot verify replica checksums");
  }
  const encoded = new TextEncoder().encode(text);
  const digest = await globalThis.crypto.subtle.digest("SHA-256", encoded);
  const bytes = Array.from(new Uint8Array(digest));
  return bytes.map((value) => value.toString(16).padStart(2, "0")).join("");
}

// src/path.ts
function normalizeReplicaPath(path) {
  return path.replace(/\\/g, "/").replace(/^\/+/, "").replace(/\/+/g, "/");
}

// src/types.ts
var DEFAULT_SETTINGS = {
  serverBaseUrl: "http://127.0.0.1:2731",
  vaultName: "",
  createIfMissing: true,
  serverRootPath: "",
  blockLocalEdits: true,
  repairIntervalMs: 6e4,
  reconnectDelayMs: 2e3,
  chatDefaultModel: "",
  chatWatchdogMs: 45e3,
  chatReconnectDelayMs: 2e3
};
function createDefaultVaultState(vaultName) {
  return {
    version: 1,
    vaultName,
    nextLsn: 0,
    files: {},
    health: {
      connectionState: "idle",
      lastSuccessfulLsn: null,
      lastSyncAtMs: null,
      lastError: null,
      unhealthyPath: null,
      resyncRequired: false
    }
  };
}

// src/replay.ts
function applyUnifiedPatch(original, patch) {
  const originalLines = original.split("\n");
  const patchLines = patch.replace(/\r\n/g, "\n").split("\n");
  const result = [];
  let originalIndex = 0;
  let patchIndex = 0;
  while (patchIndex < patchLines.length && !patchLines[patchIndex].startsWith("@@")) {
    patchIndex += 1;
  }
  while (patchIndex < patchLines.length) {
    const header = patchLines[patchIndex];
    const match = /^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@/.exec(header);
    if (!match) {
      throw new Error(`Invalid unified diff hunk header: ${header}`);
    }
    const hunkStart = Math.max(0, Number.parseInt(match[1], 10) - 1);
    while (originalIndex < hunkStart) {
      result.push(originalLines[originalIndex] ?? "");
      originalIndex += 1;
    }
    patchIndex += 1;
    while (patchIndex < patchLines.length && !patchLines[patchIndex].startsWith("@@")) {
      const line = patchLines[patchIndex] ?? "";
      if (line.startsWith("\\")) {
        patchIndex += 1;
        continue;
      }
      const marker = line[0] ?? "";
      const value = line.slice(1);
      if (marker === " ") {
        if ((originalLines[originalIndex] ?? "") !== value) {
          throw new Error(`Patch context mismatch at line ${originalIndex + 1}`);
        }
        result.push(value);
        originalIndex += 1;
      } else if (marker === "-") {
        if ((originalLines[originalIndex] ?? "") !== value) {
          throw new Error(`Patch deletion mismatch at line ${originalIndex + 1}`);
        }
        originalIndex += 1;
      } else if (marker === "+") {
        result.push(value);
      } else if (line.length === 0) {
        result.push("");
      } else {
        throw new Error(`Unsupported diff line: ${line}`);
      }
      patchIndex += 1;
    }
  }
  while (originalIndex < originalLines.length) {
    result.push(originalLines[originalIndex] ?? "");
    originalIndex += 1;
  }
  return result.join("\n");
}
function setHealthy(state, lsn) {
  return {
    ...state,
    health: {
      ...state.health,
      connectionState: "catching_up",
      lastSuccessfulLsn: lsn,
      lastSyncAtMs: Date.now(),
      lastError: null,
      unhealthyPath: null
    }
  };
}
function setRepairHealthy(state) {
  return {
    ...state,
    health: {
      ...state.health,
      connectionState: "live",
      lastSyncAtMs: Date.now(),
      lastError: null,
      unhealthyPath: null
    }
  };
}
function setUnhealthy(state, path, message) {
  return {
    ...state,
    health: {
      ...state.health,
      connectionState: "unhealthy",
      lastError: message,
      unhealthyPath: path,
      resyncRequired: true
    }
  };
}
var ReplicaReplayEngine = class {
  constructor(adapter, remote) {
    this.adapter = adapter;
    this.remote = remote;
  }
  async applyRecord(currentState, record) {
    const state = currentState.version === 1 ? currentState : createDefaultVaultState(currentState.vaultName);
    if (record.lsn < state.nextLsn) {
      return state;
    }
    const path = normalizeReplicaPath(record.path);
    if (record.action === "delete") {
      return this.applyDelete(state, record.lsn, path);
    }
    const localContent = await this.adapter.read(path);
    if (localContent !== null) {
      const localChecksum = await sha256Text(localContent);
      if (localChecksum === record.checksum) {
        const stat = await this.adapter.stat(path);
        return {
          ...setHealthy(state, record.lsn),
          nextLsn: record.lsn + 1,
          files: {
            ...state.files,
            [path]: {
              checksum: localChecksum,
              syncedMtimeMs: stat?.mtime ?? Date.now()
            }
          }
        };
      }
    }
    if (record.action === "create" && typeof record.payload.content === "string") {
      return this.writeVerifiedContent(state, record.lsn, path, record.payload.content, record.checksum);
    }
    if (record.action === "patch" && typeof record.payload.patch === "string") {
      if (localContent !== null) {
        try {
          const patched = applyUnifiedPatch(localContent, record.payload.patch);
          return await this.writeVerifiedContent(state, record.lsn, path, patched, record.checksum);
        } catch {
          return this.recoverPath(state, record.lsn, path);
        }
      }
      return this.recoverPath(state, record.lsn, path);
    }
    return this.recoverPath(state, record.lsn, path);
  }
  async scanAndRepair(currentState) {
    let state = currentState;
    const knownPaths = new Set(Object.keys(state.files));
    for (const [path, fileState] of Object.entries(state.files)) {
      const stat = await this.adapter.stat(path);
      if (stat === null) {
        state = await this.recoverPathWithoutAdvancing(state, path);
        continue;
      }
      if (stat.mtime <= fileState.syncedMtimeMs) {
        continue;
      }
      const localContent = await this.adapter.read(path);
      if (localContent === null) {
        state = await this.recoverPathWithoutAdvancing(state, path);
        continue;
      }
      const checksum = await sha256Text(localContent);
      if (checksum === fileState.checksum) {
        state = {
          ...setRepairHealthy(state),
          files: {
            ...state.files,
            [path]: {
              checksum,
              syncedMtimeMs: stat.mtime
            }
          }
        };
        continue;
      }
      state = await this.recoverPathWithoutAdvancing(state, path);
    }
    const diskPaths = await this.adapter.listFiles();
    for (const rawPath of diskPaths) {
      const path = normalizeReplicaPath(rawPath);
      if (knownPaths.has(path)) {
        continue;
      }
      const authoritative = await this.remote.queryPathState(path);
      if (!authoritative.exists) {
        await this.adapter.delete(path);
        continue;
      }
      state = await this.recoverPathWithoutAdvancing(state, path);
    }
    return state;
  }
  async applyDelete(state, lsn, path) {
    await this.adapter.delete(path);
    const files = { ...state.files };
    delete files[path];
    return {
      ...setHealthy(state, lsn),
      nextLsn: lsn + 1,
      files
    };
  }
  async writeVerifiedContent(state, lsn, path, content, expectedChecksum) {
    await this.adapter.write(path, content);
    const stat = await this.adapter.stat(path);
    const localContent = await this.adapter.read(path);
    if (stat === null || localContent === null) {
      return this.recoverPath(state, lsn, path);
    }
    const checksum = await sha256Text(localContent);
    if (checksum !== expectedChecksum) {
      return this.recoverPath(state, lsn, path);
    }
    return {
      ...setHealthy(state, lsn),
      nextLsn: Math.max(state.nextLsn, lsn + 1),
      files: {
        ...state.files,
        [path]: {
          checksum,
          syncedMtimeMs: stat.mtime
        }
      }
    };
  }
  async recoverPath(state, lsn, path) {
    const remoteState = await this.remote.fetchRawFile(path);
    if (!remoteState.exists || remoteState.deleted || remoteState.content === null || remoteState.checksum === null) {
      return this.applyDelete(state, lsn, path);
    }
    try {
      return await this.writeVerifiedContent(state, lsn, path, remoteState.content, remoteState.checksum);
    } catch (error) {
      return setUnhealthy(state, path, error instanceof Error ? error.message : String(error));
    }
  }
  async recoverPathWithoutAdvancing(state, path) {
    const remoteState = await this.remote.fetchRawFile(path);
    if (!remoteState.exists || remoteState.deleted || remoteState.content === null || remoteState.checksum === null) {
      await this.adapter.delete(path);
      const files = { ...state.files };
      delete files[path];
      return {
        ...setRepairHealthy(state),
        files
      };
    }
    try {
      await this.adapter.write(path, remoteState.content);
      const stat = await this.adapter.stat(path);
      const localContent = await this.adapter.read(path);
      if (stat === null || localContent === null) {
        throw new Error(`Failed to verify repaired file ${path}`);
      }
      const checksum = await sha256Text(localContent);
      if (checksum !== remoteState.checksum) {
        throw new Error(`Checksum mismatch after repairing ${path}`);
      }
      return {
        ...setRepairHealthy(state),
        files: {
          ...state.files,
          [path]: {
            checksum,
            syncedMtimeMs: stat.mtime
          }
        }
      };
    } catch (error) {
      return setUnhealthy(state, path, error instanceof Error ? error.message : String(error));
    }
  }
};

// src/state.ts
function normalizeState(input, vaultName) {
  const base = createDefaultVaultState(vaultName);
  return {
    version: 1,
    vaultName: input?.vaultName || vaultName,
    nextLsn: typeof input?.nextLsn === "number" ? input.nextLsn : base.nextLsn,
    files: input?.files ?? base.files,
    health: {
      ...base.health,
      ...input?.health ?? {}
    }
  };
}
var ReplicaStateRepository = class {
  constructor(plugin) {
    this.plugin = plugin;
  }
  async loadPluginData() {
    const loaded = await this.plugin.loadData();
    return loaded ?? {};
  }
  async loadSettings(defaultVaultName) {
    const pluginData = await this.loadPluginData();
    return {
      ...DEFAULT_SETTINGS,
      vaultName: defaultVaultName,
      ...pluginData.settings ?? {}
    };
  }
  async saveSettings(settings) {
    const pluginData = await this.loadPluginData();
    await this.plugin.saveData({
      ...pluginData,
      settings
    });
  }
  async loadState(vaultName) {
    const pluginData = await this.loadPluginData();
    return normalizeState(pluginData.state, vaultName);
  }
  async saveState(state) {
    const pluginData = await this.loadPluginData();
    await this.plugin.saveData({
      ...pluginData,
      state
    });
  }
};

// src/syncClient.ts
var import_obsidian6 = require("obsidian");

// src/replayQueue.ts
var ReplayQueue = class {
  tail = Promise.resolve();
  enqueue(task, onError) {
    this.tail = this.tail.catch(() => void 0).then(task).catch(async (error) => {
      await onError(error);
    });
  }
};

// src/syncClient.ts
var ReplicaSyncService = class {
  constructor(repository, settings, replayEngine, onStateChange) {
    this.repository = repository;
    this.settings = settings;
    this.replayEngine = replayEngine;
    this.onStateChange = onStateChange;
  }
  state = null;
  socket = null;
  isStopping = false;
  reconnectHandle = null;
  replayQueue = new ReplayQueue();
  pendingRawFile = /* @__PURE__ */ new Map();
  pendingPathState = /* @__PURE__ */ new Map();
  replicationPaused = false;
  async start() {
    if (this.socket !== null) {
      return;
    }
    this.isStopping = false;
    this.replicationPaused = false;
    const settings = this.settings();
    this.state = await this.repository.loadState(settings.vaultName);
    this.state = {
      ...this.state,
      health: {
        ...this.state.health,
        connectionState: "connecting",
        lastError: null
      }
    };
    await this.persistState();
    const session = await this.startSession(settings, this.state.nextLsn);
    await this.connectSocket(settings, session);
  }
  stop() {
    this.isStopping = true;
    if (this.reconnectHandle !== null) {
      window.clearTimeout(this.reconnectHandle);
      this.reconnectHandle = null;
    }
    this.socket?.close();
    this.socket = null;
  }
  async repairNow() {
    if (this.state === null) {
      this.state = await this.repository.loadState(this.settings().vaultName);
    }
    this.state = await this.replayEngine.scanAndRepair(this.state);
    if (!this.state.health.resyncRequired) {
      this.state = {
        ...this.state,
        health: {
          ...this.state.health,
          connectionState: "live"
        }
      };
    }
    await this.persistState();
    if (this.replicationPaused) {
      await this.resumeReplication(this.state.nextLsn);
    }
  }
  async fetchRawFile(path) {
    return this.sendRequest("fetch_raw_file", path, this.pendingRawFile);
  }
  async queryPathState(path) {
    return this.sendRequest("query_path_state", path, this.pendingPathState);
  }
  async startSession(settings, nextLsn) {
    const response = await (0, import_obsidian6.requestUrl)({
      url: `${settings.serverBaseUrl}/replica/sessions`,
      method: "POST",
      contentType: "application/json",
      body: JSON.stringify({
        vault_name: settings.vaultName,
        next_lsn: nextLsn,
        create_if_missing: settings.createIfMissing,
        root_path: settings.serverRootPath || void 0
      })
    });
    return response.json;
  }
  async connectSocket(settings, session) {
    await new Promise((resolve, reject) => {
      const wsUrl = `${settings.serverBaseUrl.replace(/^http/, "ws")}${session.websocket_url}`;
      const socket = new WebSocket(wsUrl);
      this.socket = socket;
      socket.addEventListener("open", () => resolve(), { once: true });
      socket.addEventListener("error", () => reject(new Error("Replica sync socket error")), { once: true });
      socket.addEventListener("message", (event) => {
        const payload = JSON.parse(String(event.data));
        this.handleSocketMessage(payload);
      });
      socket.addEventListener("close", () => {
        this.socket = null;
        if (this.isStopping) {
          return;
        }
        void this.scheduleReconnect();
      });
      socket.addEventListener("error", () => {
        new import_obsidian6.Notice("Replica sync socket error");
      });
    });
  }
  handleSocketMessage(message) {
    if (message.type === "log_record") {
      this.replayQueue.enqueue(
        async () => {
          await this.handleReplayMessage(message);
        },
        async (error) => {
          await this.handleReplayError(error);
        }
      );
      return;
    }
    void this.handleControlMessage(message);
  }
  async handleReplayMessage(message) {
    if (this.state === null) {
      this.state = await this.repository.loadState(this.settings().vaultName);
    }
    this.state = await this.replayEngine.applyRecord(this.state, message);
    await this.persistState();
    if (this.replicationPaused) {
      await this.resumeReplication(this.state.nextLsn);
      return;
    }
    this.socket?.send(JSON.stringify({ type: "ack", lsn: message.lsn }));
  }
  async handleReplayError(error) {
    console.error("Replica replay handler failed", error);
    if (this.state === null) {
      this.state = await this.repository.loadState(this.settings().vaultName);
    }
    this.replicationPaused = true;
    this.state = {
      ...this.state,
      health: {
        ...this.state.health,
        connectionState: "unhealthy",
        lastError: error instanceof Error ? error.message : String(error),
        resyncRequired: true
      }
    };
    await this.persistState();
  }
  async handleControlMessage(message) {
    if (this.state === null) {
      this.state = await this.repository.loadState(this.settings().vaultName);
    }
    if (message.type === "sync_hello") {
      this.replicationPaused = false;
      this.state = {
        ...this.state,
        health: {
          ...this.state.health,
          connectionState: "catching_up",
          resyncRequired: false
        }
      };
      await this.persistState();
      return;
    }
    if (message.type === "replication_paused") {
      this.replicationPaused = true;
      return;
    }
    if (message.type === "replication_resumed") {
      this.replicationPaused = false;
      return;
    }
    if (message.type === "sync_caught_up") {
      this.replicationPaused = false;
      this.state = {
        ...this.state,
        health: {
          ...this.state.health,
          connectionState: "live",
          lastError: null
        }
      };
      await this.persistState();
      return;
    }
    if (message.type === "raw_file_response") {
      const path = String(message.path ?? "");
      const pending = this.pendingRawFile.get(path);
      if (pending) {
        this.pendingRawFile.delete(path);
        pending.resolve(message);
      }
      return;
    }
    if (message.type === "path_state_response") {
      const path = String(message.path ?? "");
      const pending = this.pendingPathState.get(path);
      if (pending) {
        this.pendingPathState.delete(path);
        pending.resolve(message);
      }
      return;
    }
    if (message.type === "resync_required" || message.type === "error") {
      this.replicationPaused = true;
      this.state = {
        ...this.state,
        health: {
          ...this.state.health,
          connectionState: "unhealthy",
          lastError: String(message.reason ?? message.message ?? "Replica sync failed"),
          resyncRequired: true
        }
      };
      await this.persistState();
      new import_obsidian6.Notice(`Replica sync requires attention: ${this.state.health.lastError}`);
    }
  }
  async persistState() {
    if (this.state === null) {
      return;
    }
    await this.repository.saveState(this.state);
    await this.onStateChange(this.state);
  }
  async scheduleReconnect() {
    if (this.isStopping) {
      return;
    }
    if (this.state !== null) {
      this.state = {
        ...this.state,
        health: {
          ...this.state.health,
          connectionState: "connecting"
        }
      };
      await this.persistState();
    }
    this.reconnectHandle = window.setTimeout(() => {
      void this.start();
    }, this.settings().reconnectDelayMs);
  }
  async resumeReplication(nextLsn) {
    if (this.socket === null || this.socket.readyState !== WebSocket.OPEN) {
      return;
    }
    this.replicationPaused = false;
    this.socket.send(JSON.stringify({ type: "resume_replication", next_lsn: nextLsn }));
  }
  sendRequest(frameType, path, pendingMap) {
    if (this.socket === null || this.socket.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error("Replica sync socket is not open"));
    }
    return new Promise((resolve, reject) => {
      this.replicationPaused = true;
      pendingMap.set(path, { resolve, reject });
      this.socket?.send(JSON.stringify({ type: frameType, path }));
      window.setTimeout(() => {
        const pending = pendingMap.get(path);
        if (!pending) {
          return;
        }
        pendingMap.delete(path);
        pending.reject(new Error(`Timed out waiting for ${frameType}(${path})`));
      }, 5e3);
    });
  }
};

// src/main.ts
var ObsidianVaultAdapter = class {
  constructor(app) {
    this.app = app;
  }
  async read(path) {
    const file = this.app.vault.getAbstractFileByPath((0, import_obsidian7.normalizePath)(path));
    if (!(file instanceof import_obsidian7.TFile)) {
      return null;
    }
    return this.app.vault.cachedRead(file);
  }
  async write(path, content) {
    const normalized = (0, import_obsidian7.normalizePath)(path);
    const existing = this.app.vault.getAbstractFileByPath(normalized);
    if (existing instanceof import_obsidian7.TFile) {
      await this.app.vault.modify(existing, content);
      await this.refreshOpenLeaves(existing, content);
      this.app.vault.trigger("modify", existing);
      return;
    }
    await this.ensureFolders(normalized);
    const created = await this.app.vault.create(normalized, content);
    await this.refreshOpenLeaves(created, content);
    this.app.vault.trigger("modify", created);
  }
  async delete(path) {
    const file = this.app.vault.getAbstractFileByPath((0, import_obsidian7.normalizePath)(path));
    if (file instanceof import_obsidian7.TFile) {
      await this.closeOpenLeaves(file);
      await this.app.vault.delete(file, true);
      this.app.workspace.trigger("layout-change");
      return;
    }
    if (file) {
      await this.app.vault.delete(file, true);
    }
  }
  async stat(path) {
    const stat = await this.app.vault.adapter.stat((0, import_obsidian7.normalizePath)(path));
    if (!stat) {
      return null;
    }
    return { mtime: stat.mtime };
  }
  async listFiles() {
    return this.app.vault.getFiles().map((file) => file.path);
  }
  async ensureFolders(path) {
    const parts = (0, import_obsidian7.normalizePath)(path).split("/").slice(0, -1);
    let current = "";
    for (const part of parts) {
      current = current ? `${current}/${part}` : part;
      if (this.app.vault.getAbstractFileByPath(current)) {
        continue;
      }
      await this.app.vault.createFolder(current);
    }
  }
  async refreshOpenLeaves(file, content) {
    const matchingLeaves = this.app.workspace.getLeavesOfType("markdown").filter((leaf) => leaf.view instanceof import_obsidian7.MarkdownView && leaf.view.file?.path === file.path);
    for (const leaf of matchingLeaves) {
      const view = leaf.view;
      if (!(view instanceof import_obsidian7.MarkdownView)) {
        continue;
      }
      const scroll = view.currentMode.getScroll();
      view.data = content;
      view.setViewData(content, false);
      view.currentMode.set(content, false);
      await view.onLoadFile(file);
      view.currentMode.applyScroll(scroll);
      view.previewMode?.rerender(true);
    }
    if (matchingLeaves.length > 0) {
      new import_obsidian7.Notice(`Replica refreshed ${file.path}`, 2e3);
      this.app.workspace.trigger("layout-change");
    }
  }
  async closeOpenLeaves(file) {
    const matchingLeaves = this.app.workspace.getLeavesOfType("markdown").filter((leaf) => leaf.view instanceof import_obsidian7.MarkdownView && leaf.view.file?.path === file.path);
    for (const leaf of matchingLeaves) {
      await leaf.setViewState({ type: "empty" });
    }
  }
};
var ReplicaSettingTab = class extends import_obsidian7.PluginSettingTab {
  constructor(app, plugin) {
    super(app, plugin);
    this.plugin = plugin;
  }
  display() {
    const { containerEl } = this;
    containerEl.empty();
    new import_obsidian7.Setting(containerEl).setName("Server base URL").setDesc("Replica session endpoint for the Sheaf server.").addText(
      (text) => text.setValue(this.plugin.settings.serverBaseUrl).onChange(async (value) => {
        this.plugin.settings.serverBaseUrl = value.trim() || DEFAULT_SETTINGS.serverBaseUrl;
        await this.plugin.persistSettings();
      })
    );
    new import_obsidian7.Setting(containerEl).setName("Vault name").setDesc("Logical server-side vault name used by replica sync.").addText(
      (text) => text.setValue(this.plugin.settings.vaultName).onChange(async (value) => {
        this.plugin.settings.vaultName = value.trim() || this.app.vault.getName();
        await this.plugin.persistSettings();
      })
    );
    new import_obsidian7.Setting(containerEl).setName("Server root path").setDesc("Required when the server must create the replica vault on first sync.").addText(
      (text) => text.setValue(this.plugin.settings.serverRootPath).onChange(async (value) => {
        this.plugin.settings.serverRootPath = value.trim();
        await this.plugin.persistSettings();
      })
    );
    new import_obsidian7.Setting(containerEl).setName("Create missing vault automatically").addToggle(
      (toggle) => toggle.setValue(this.plugin.settings.createIfMissing).onChange(async (value) => {
        this.plugin.settings.createIfMissing = value;
        await this.plugin.persistSettings();
      })
    );
    new import_obsidian7.Setting(containerEl).setName("Block local edits").setDesc("Reject typing, paste, undo, and redo in replicated notes.").addToggle(
      (toggle) => toggle.setValue(this.plugin.settings.blockLocalEdits).onChange(async (value) => {
        this.plugin.settings.blockLocalEdits = value;
        await this.plugin.persistSettings();
      })
    );
    new import_obsidian7.Setting(containerEl).setName("Chat default model").setDesc("Choose which model new chat messages should use.").addDropdown((dropdown) => {
      dropdown.addOption("", "Server default");
      const models = this.plugin.availableChatModels;
      for (const model of models) {
        const suffix = model.is_default ? " (default)" : "";
        dropdown.addOption(model.name, `${model.name} \xB7 ${model.provider}${suffix}`);
      }
      if (this.plugin.settings.chatDefaultModel && !models.some((model) => model.name === this.plugin.settings.chatDefaultModel)) {
        dropdown.addOption(this.plugin.settings.chatDefaultModel, `${this.plugin.settings.chatDefaultModel} \xB7 custom`);
      }
      dropdown.setValue(this.plugin.settings.chatDefaultModel).onChange(async (value) => {
        this.plugin.settings.chatDefaultModel = value.trim();
        await this.plugin.persistSettings();
      });
    }).addButton((button) => {
      button.setButtonText("Refresh").onClick(async () => {
        await this.plugin.refreshAvailableChatModels(true);
        this.display();
      });
    });
    new import_obsidian7.Setting(containerEl).setName("Chat reconnect delay (ms)").setDesc("Delay before retrying after a chat disconnect or conflict.").addText(
      (text) => text.setValue(String(this.plugin.settings.chatReconnectDelayMs)).onChange(async (value) => {
        const parsed = Number.parseInt(value, 10);
        this.plugin.settings.chatReconnectDelayMs = Number.isFinite(parsed) ? Math.max(parsed, 250) : DEFAULT_SETTINGS.chatReconnectDelayMs;
        await this.plugin.persistSettings();
      })
    );
    new import_obsidian7.Setting(containerEl).setName("Chat watchdog timeout (ms)").setDesc("Reconnect the chat pane if no websocket frames arrive within this window.").addText(
      (text) => text.setValue(String(this.plugin.settings.chatWatchdogMs)).onChange(async (value) => {
        const parsed = Number.parseInt(value, 10);
        this.plugin.settings.chatWatchdogMs = Number.isFinite(parsed) ? Math.max(parsed, 5e3) : DEFAULT_SETTINGS.chatWatchdogMs;
        await this.plugin.persistSettings();
      })
    );
  }
};
var SheafObsidianReplicaPlugin = class extends import_obsidian7.Plugin {
  settings = { ...DEFAULT_SETTINGS };
  availableChatModels = [];
  stateRepository;
  syncService = null;
  chatService;
  latestState = null;
  repairTimer = null;
  settingTab;
  async onload() {
    this.stateRepository = new ReplicaStateRepository(this);
    this.settings = await this.stateRepository.loadSettings(this.app.vault.getName());
    this.latestState = await this.stateRepository.loadState(this.settings.vaultName);
    const adapter = new ObsidianVaultAdapter(this.app);
    const remoteReader = {
      fetchRawFile: async (path) => {
        if (!this.syncService) {
          throw new Error("Replica sync service is not ready");
        }
        return this.syncService.fetchRawFile(path);
      },
      queryPathState: async (path) => {
        if (!this.syncService) {
          throw new Error("Replica sync service is not ready");
        }
        return this.syncService.queryPathState(path);
      }
    };
    const replayEngine = new ReplicaReplayEngine(adapter, remoteReader);
    this.syncService = new ReplicaSyncService(
      this.stateRepository,
      () => this.settings,
      replayEngine,
      async (state) => {
        this.latestState = state;
      }
    );
    this.registerEditorExtension(
      createReplicaEditBlocker(() => {
        if (!this.settings.blockLocalEdits) {
          return false;
        }
        const activeFile = this.app.workspace.getActiveViewOfType(import_obsidian7.MarkdownView)?.file;
        if (!(activeFile instanceof import_obsidian7.TFile)) {
          return false;
        }
        return Boolean(this.latestState?.files[(0, import_obsidian7.normalizePath)(activeFile.path)]);
      })
    );
    this.chatService = new ChatService({
      settings: () => this.settings,
      openSettings: () => this.openPluginSettings()
    });
    this.registerView(SHEAF_CHAT_VIEW_TYPE, (leaf) => new SheafChatView(leaf, this.chatService));
    this.settingTab = new ReplicaSettingTab(this.app, this);
    this.addSettingTab(this.settingTab);
    void this.refreshAvailableChatModels(false);
    this.addCommand({
      id: "replica-sync-now",
      name: "Sync replica now",
      callback: async () => {
        try {
          await this.syncService?.start();
          new import_obsidian7.Notice("Replica sync started");
        } catch (error) {
          new import_obsidian7.Notice(`Replica sync failed: ${error instanceof Error ? error.message : String(error)}`);
        }
      }
    });
    this.addCommand({
      id: "replica-repair-now",
      name: "Repair replica now",
      callback: async () => {
        try {
          await this.syncService?.repairNow();
          new import_obsidian7.Notice("Replica repair complete");
        } catch (error) {
          new import_obsidian7.Notice(`Replica repair failed: ${error instanceof Error ? error.message : String(error)}`);
        }
      }
    });
    this.addCommand({
      id: "replica-show-status",
      name: "Show replica status",
      callback: async () => {
        const state = this.latestState ?? await this.stateRepository.loadState(this.settings.vaultName);
        const health = state.health;
        new import_obsidian7.Notice(
          `Replica ${health.connectionState}; next LSN ${state.nextLsn}; last good ${health.lastSuccessfulLsn ?? "none"}`,
          6e3
        );
      }
    });
    this.addCommand({
      id: "open-chat-pane",
      name: "Open Sheaf chat",
      callback: async () => {
        await this.activateChatView();
      }
    });
    try {
      await this.syncService.start();
    } catch (error) {
      new import_obsidian7.Notice(`Replica sync failed to start: ${error instanceof Error ? error.message : String(error)}`);
    }
    this.repairTimer = window.setInterval(() => {
      void this.syncService?.repairNow();
    }, this.settings.repairIntervalMs);
    this.register(() => {
      if (this.repairTimer !== null) {
        window.clearInterval(this.repairTimer);
        this.repairTimer = null;
      }
    });
  }
  onunload() {
    void this.chatService?.deactivateView();
    this.app.workspace.getLeavesOfType(SHEAF_CHAT_VIEW_TYPE).forEach((leaf) => leaf.detach());
    this.syncService?.stop();
    if (this.repairTimer !== null) {
      window.clearInterval(this.repairTimer);
      this.repairTimer = null;
    }
  }
  async persistSettings() {
    await this.stateRepository.saveSettings(this.settings);
  }
  async refreshAvailableChatModels(showFailureNotice) {
    try {
      const response = await (0, import_obsidian7.requestUrl)({
        url: `${this.settings.serverBaseUrl}/models`,
        method: "GET"
      });
      const payload = response.json;
      const models = Array.isArray(payload.models) ? payload.models : [];
      this.availableChatModels = models.filter((item) => typeof item?.name === "string").map((item) => ({
        name: String(item.name),
        provider: typeof item.provider === "string" ? item.provider : "unknown",
        is_default: item.is_default === true
      }));
      this.settingTab?.display();
    } catch (error) {
      if (showFailureNotice) {
        new import_obsidian7.Notice(`Failed to load models: ${error instanceof Error ? error.message : String(error)}`);
      }
    }
  }
  async activateChatView() {
    const existing = this.app.workspace.getLeavesOfType(SHEAF_CHAT_VIEW_TYPE)[0];
    const leaf = existing ?? this.app.workspace.getLeaf(true);
    await leaf.setViewState({ type: SHEAF_CHAT_VIEW_TYPE, active: true });
    this.app.workspace.revealLeaf(leaf);
  }
  openPluginSettings() {
    const settingAPI = this.app.setting;
    settingAPI?.open?.();
    settingAPI?.openTabById?.(this.manifest.id);
  }
};
