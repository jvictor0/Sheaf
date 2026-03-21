import { Notice, requestUrl } from "obsidian";

import { ReplicaReplayEngine } from "./replay";
import { ReplayQueue } from "./replayQueue";
import { ReplicaStateRepository } from "./state";
import {
  ReplicaLogRecord,
  ReplicaPathState,
  ReplicaPluginSettings,
  ReplicaSessionEnvelope,
  ReplicaSessionResponse,
  ReplicaVaultState,
} from "./types";

type PendingResolver<T> = {
  resolve: (value: T) => void;
  reject: (reason?: unknown) => void;
};

export class ReplicaSyncService {
  private state: ReplicaVaultState | null = null;
  private socket: WebSocket | null = null;
  private isStopping = false;
  private reconnectHandle: number | null = null;
  private readonly replayQueue = new ReplayQueue();
  private pendingRawFile = new Map<string, PendingResolver<ReplicaPathState>>();
  private pendingPathState = new Map<string, PendingResolver<Omit<ReplicaPathState, "content">>>();
  private replicationPaused = false;

  constructor(
    private readonly repository: ReplicaStateRepository,
    private readonly settings: () => ReplicaPluginSettings,
    private readonly replayEngine: ReplicaReplayEngine,
    private readonly onStateChange: (state: ReplicaVaultState) => Promise<void>,
  ) {}

  async start(): Promise<void> {
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
        lastError: null,
      },
    };
    await this.persistState();

    const session = await this.startSession(settings, this.state.nextLsn);
    await this.connectSocket(settings, session);
  }

  stop(): void {
    this.isStopping = true;
    if (this.reconnectHandle !== null) {
      window.clearTimeout(this.reconnectHandle);
      this.reconnectHandle = null;
    }
    this.socket?.close();
    this.socket = null;
  }

  async repairNow(): Promise<void> {
    if (this.state === null) {
      this.state = await this.repository.loadState(this.settings().vaultName);
    }
    this.state = await this.replayEngine.scanAndRepair(this.state);
    if (!this.state.health.resyncRequired) {
      this.state = {
        ...this.state,
        health: {
          ...this.state.health,
          connectionState: "live",
        },
      };
    }
    await this.persistState();
    if (this.replicationPaused) {
      await this.resumeReplication(this.state.nextLsn);
    }
  }

  async fetchRawFile(path: string): Promise<ReplicaPathState> {
    return this.sendRequest("fetch_raw_file", path, this.pendingRawFile);
  }

  async queryPathState(path: string): Promise<Omit<ReplicaPathState, "content">> {
    return this.sendRequest("query_path_state", path, this.pendingPathState);
  }

  private async startSession(settings: ReplicaPluginSettings, nextLsn: number): Promise<ReplicaSessionResponse> {
    const response = await requestUrl({
      url: `${settings.serverBaseUrl}/replica/sessions`,
      method: "POST",
      contentType: "application/json",
      body: JSON.stringify({
        vault_name: settings.vaultName,
        next_lsn: nextLsn,
        create_if_missing: settings.createIfMissing,
        root_path: settings.serverRootPath || undefined,
      }),
    });
    return response.json as ReplicaSessionResponse;
  }

  private async connectSocket(settings: ReplicaPluginSettings, session: ReplicaSessionResponse): Promise<void> {
    await new Promise<void>((resolve, reject) => {
      const wsUrl = `${settings.serverBaseUrl.replace(/^http/, "ws")}${session.websocket_url}`;
      const socket = new WebSocket(wsUrl);
      this.socket = socket;

      socket.addEventListener("open", () => resolve(), { once: true });
      socket.addEventListener("error", () => reject(new Error("Replica sync socket error")), { once: true });

      socket.addEventListener("message", (event) => {
        const payload = JSON.parse(String(event.data)) as ReplicaSessionEnvelope;
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
        new Notice("Replica sync socket error");
      });
    });
  }

  private handleSocketMessage(message: ReplicaSessionEnvelope): void {
    if (message.type === "log_record") {
      this.replayQueue.enqueue(
        async () => {
          await this.handleReplayMessage(message as ReplicaLogRecord & ReplicaSessionEnvelope);
        },
        async (error) => {
          await this.handleReplayError(error);
        },
      );
      return;
    }
    void this.handleControlMessage(message);
  }

  private async handleReplayMessage(message: ReplicaLogRecord & ReplicaSessionEnvelope): Promise<void> {
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

  private async handleReplayError(error: unknown): Promise<void> {
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
        resyncRequired: true,
      },
    };
    await this.persistState();
  }

  private async handleControlMessage(message: ReplicaSessionEnvelope): Promise<void> {
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
          resyncRequired: false,
        },
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
          lastError: null,
        },
      };
      await this.persistState();
      return;
    }

    if (message.type === "raw_file_response") {
      const path = String(message.path ?? "");
      const pending = this.pendingRawFile.get(path);
      if (pending) {
        this.pendingRawFile.delete(path);
        pending.resolve(message as unknown as ReplicaPathState);
      }
      return;
    }

    if (message.type === "path_state_response") {
      const path = String(message.path ?? "");
      const pending = this.pendingPathState.get(path);
      if (pending) {
        this.pendingPathState.delete(path);
        pending.resolve(message as unknown as Omit<ReplicaPathState, "content">);
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
          resyncRequired: true,
        },
      };
      await this.persistState();
      new Notice(`Replica sync requires attention: ${this.state.health.lastError}`);
    }
  }

  private async persistState(): Promise<void> {
    if (this.state === null) {
      return;
    }
    await this.repository.saveState(this.state);
    await this.onStateChange(this.state);
  }

  private async scheduleReconnect(): Promise<void> {
    if (this.isStopping) {
      return;
    }
    if (this.state !== null) {
      this.state = {
        ...this.state,
        health: {
          ...this.state.health,
          connectionState: "connecting",
        },
      };
      await this.persistState();
    }
    this.reconnectHandle = window.setTimeout(() => {
      void this.start();
    }, this.settings().reconnectDelayMs);
  }

  private async resumeReplication(nextLsn: number): Promise<void> {
    if (this.socket === null || this.socket.readyState !== WebSocket.OPEN) {
      return;
    }
    this.replicationPaused = false;
    this.socket.send(JSON.stringify({ type: "resume_replication", next_lsn: nextLsn }));
  }

  private sendRequest<T>(
    frameType: string,
    path: string,
    pendingMap: Map<string, PendingResolver<T>>,
  ): Promise<T> {
    if (this.socket === null || this.socket.readyState !== WebSocket.OPEN) {
      return Promise.reject(new Error("Replica sync socket is not open"));
    }
    return new Promise<T>((resolve, reject) => {
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
      }, 5_000);
    });
  }
}
