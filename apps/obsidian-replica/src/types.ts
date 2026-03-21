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
