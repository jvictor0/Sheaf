export type ReplicaConnectionState = "idle" | "connecting" | "catching_up" | "live" | "unhealthy";

export interface ReplicaFileState {
  checksum: string;
  syncedMtimeMs: number;
}

export interface ReplicaHealthState {
  connectionState: ReplicaConnectionState;
  lastSuccessfulLsn: number | null;
  lastSyncAtMs: number | null;
  lastError: string | null;
  unhealthyPath: string | null;
  resyncRequired: boolean;
}

export interface ReplicaVaultState {
  version: 1;
  vaultName: string;
  nextLsn: number;
  files: Record<string, ReplicaFileState>;
  health: ReplicaHealthState;
}

export interface ReplicaPluginSettings {
  serverBaseUrl: string;
  vaultName: string;
  createIfMissing: boolean;
  serverRootPath: string;
  blockLocalEdits: boolean;
  repairIntervalMs: number;
  reconnectDelayMs: number;
  chatDefaultModel: string;
  chatWatchdogMs: number;
  chatReconnectDelayMs: number;
}

export interface ReplicaPluginData {
  settings?: Partial<ReplicaPluginSettings>;
  state?: Partial<ReplicaVaultState>;
}

export interface ReplicaLogRecord {
  lsn: number;
  path: string;
  action: "create" | "patch" | "delete";
  checksum: string;
  payload: {
    content?: string;
    patch?: string;
  };
  recorded_at?: string;
}

export interface ReplicaPathState {
  path: string;
  exists: boolean;
  deleted: boolean;
  checksum: string | null;
  content: string | null;
  last_lsn: number | null;
}

export interface ReplicaSessionEnvelope {
  protocol_version: number;
  type: string;
  session_id: string;
  server_time: string;
  [key: string]: unknown;
}

export interface ReplicaSessionResponse {
  session_id: string;
  websocket_url: string;
  accepted_protocol_version: number;
  vault_id: number;
  vault_name: string;
  root_path: string;
  created: boolean;
  next_lsn: number;
}

export const DEFAULT_SETTINGS: ReplicaPluginSettings = {
  serverBaseUrl: "http://127.0.0.1:2731",
  vaultName: "",
  createIfMissing: true,
  serverRootPath: "",
  blockLocalEdits: true,
  repairIntervalMs: 60_000,
  reconnectDelayMs: 2_000,
  chatDefaultModel: "",
  chatWatchdogMs: 45_000,
  chatReconnectDelayMs: 2_000,
};

export function createDefaultVaultState(vaultName: string): ReplicaVaultState {
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
      resyncRequired: false,
    },
  };
}

export type ChatConnectionState = "idle" | "connecting" | "replaying" | "live" | "error";
export type ChatScreen = "threads" | "conversation";
export type ChatJSONValue =
  | string
  | number
  | boolean
  | null
  | ChatJSONValue[]
  | { [key: string]: ChatJSONValue };

export interface ChatToolCall {
  id: string;
  name: string;
  args: Record<string, ChatJSONValue>;
  result: string;
  isError: boolean;
}

export interface ChatThreadSummary {
  thread_id: string;
  name: string;
  prev_thread_id: string | null;
  start_turn_id: string | null;
  is_archived: boolean;
  tail_turn_id: string | null;
  created_at: string | null;
  updated_at: string | null;
}

export interface ChatModelOption {
  name: string;
  provider: string;
  is_default: boolean;
}

export interface ChatCommittedTurn {
  id: string;
  thread_id: string;
  prev_turn_id: string | null;
  speaker: "user" | "assistant" | "system" | string;
  message_text: string;
  model_name: string | null;
  created_at: string | null;
  tool_calls: ChatToolCall[];
}

export interface ChatPendingSend {
  clientMessageID: string;
  text: string;
  responseToTurnID: string | null;
  localMessageID: string;
  queueID: number | null;
}

export interface ChatStreamingAssistantTurn {
  queueID: number;
  text: string;
}

export interface ChatContextBudget {
  contextSize: number;
  maxContextSize: number;
}

export interface ChatEnterResponse {
  session_id: string;
  websocket_url: string;
  accepted_protocol_version: number;
}

export interface ChatDurableAckEvent {
  type: "durable_ack";
  queueID: number;
  clientMessageID: string | null;
}

export interface ChatAssistantTokenEvent {
  type: "assistant_token";
  queueID: number;
  chunk: string;
}

export interface ChatCommittedTurnEvent {
  type: "committed_turn";
  turn: ChatCommittedTurn;
}

export interface ChatHandshakeBeginEvent {
  type: "handshake_begin";
  threadID: string | null;
}

export interface ChatHandshakeReadyEvent {
  type: "handshake_ready";
}

export interface ChatTurnFinalizedEvent {
  type: "turn_finalized";
  queueID: number;
  turnID: string | null;
}

export interface ChatConflictEvent {
  type: "execution_conflict";
  queueID: number;
  expectedTailTurnID: string | null;
  actualTailTurnID: string | null;
}

export interface ChatContextBudgetEvent {
  type: "context_budget";
  context: ChatContextBudget;
}

export interface ChatTurnEventEvent {
  type: "turn_event";
  queueID: number;
  event: string;
  trace: string | null;
  traceKind: string | null;
  payload: ChatJSONValue | null;
}

export interface ChatHeartbeatEvent {
  type: "heartbeat";
  intervalSeconds: number | null;
}

export interface ChatErrorEvent {
  type: "error";
  message: string;
  queueID: number | null;
  fatal: boolean;
}

export interface ChatClosedEvent {
  type: "closed";
  code: number;
  reason: string | null;
}

export type ChatTransportEvent =
  | ChatDurableAckEvent
  | ChatAssistantTokenEvent
  | ChatCommittedTurnEvent
  | ChatHandshakeBeginEvent
  | ChatHandshakeReadyEvent
  | ChatTurnFinalizedEvent
  | ChatConflictEvent
  | ChatContextBudgetEvent
  | ChatTurnEventEvent
  | ChatHeartbeatEvent
  | ChatErrorEvent
  | ChatClosedEvent;

export type ChatTranscriptItem =
  | {
      kind: "tool_call";
      id: string;
      text: string;
      tone: "normal" | "error";
    }
  | {
      kind: "committed";
      id: string;
      role: "user" | "assistant" | "system";
      text: string;
    }
  | {
      kind: "pending";
      id: string;
      role: "user";
      text: string;
    }
  | {
      kind: "streaming";
      id: string;
      role: "assistant";
      text: string;
      queueID: number;
    };

export interface ChatThreadSessionState {
  thread: ChatThreadSummary;
  committedTurns: ChatCommittedTurn[];
  pendingSends: ChatPendingSend[];
  streamingByQueue: Record<number, ChatStreamingAssistantTurn>;
  lastCommittedTurnID: string | null;
  lastFrameAtMs: number | null;
  errorMessage: string | null;
  connectionState: ChatConnectionState;
  thinkingActive: boolean;
  statusMessage: string | null;
  contextBudget: ChatContextBudget | null;
  transcriptItems: ChatTranscriptItem[];
}

export interface ChatThreadListState {
  loading: boolean;
  creating: boolean;
  errorMessage: string | null;
  threads: ChatThreadSummary[];
}

export interface ChatViewState {
  screen: ChatScreen;
  threadList: ChatThreadListState;
  activeThreadId: string | null;
  activeSession: ChatThreadSessionState | null;
}
