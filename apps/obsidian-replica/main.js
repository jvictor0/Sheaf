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
var import_obsidian2 = require("obsidian");

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
  reconnectDelayMs: 2e3
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
var import_obsidian = require("obsidian");

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
    const response = await (0, import_obsidian.requestUrl)({
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
        new import_obsidian.Notice("Replica sync socket error");
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
      new import_obsidian.Notice(`Replica sync requires attention: ${this.state.health.lastError}`);
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
    const file = this.app.vault.getAbstractFileByPath((0, import_obsidian2.normalizePath)(path));
    if (!(file instanceof import_obsidian2.TFile)) {
      return null;
    }
    return this.app.vault.cachedRead(file);
  }
  async write(path, content) {
    const normalized = (0, import_obsidian2.normalizePath)(path);
    const existing = this.app.vault.getAbstractFileByPath(normalized);
    if (existing instanceof import_obsidian2.TFile) {
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
    const file = this.app.vault.getAbstractFileByPath((0, import_obsidian2.normalizePath)(path));
    if (file instanceof import_obsidian2.TFile) {
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
    const stat = await this.app.vault.adapter.stat((0, import_obsidian2.normalizePath)(path));
    if (!stat) {
      return null;
    }
    return { mtime: stat.mtime };
  }
  async listFiles() {
    return this.app.vault.getFiles().map((file) => file.path);
  }
  async ensureFolders(path) {
    const parts = (0, import_obsidian2.normalizePath)(path).split("/").slice(0, -1);
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
    const matchingLeaves = this.app.workspace.getLeavesOfType("markdown").filter((leaf) => leaf.view instanceof import_obsidian2.MarkdownView && leaf.view.file?.path === file.path);
    for (const leaf of matchingLeaves) {
      const view = leaf.view;
      if (!(view instanceof import_obsidian2.MarkdownView)) {
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
      new import_obsidian2.Notice(`Replica refreshed ${file.path}`, 2e3);
      this.app.workspace.trigger("layout-change");
    }
  }
  async closeOpenLeaves(file) {
    const matchingLeaves = this.app.workspace.getLeavesOfType("markdown").filter((leaf) => leaf.view instanceof import_obsidian2.MarkdownView && leaf.view.file?.path === file.path);
    for (const leaf of matchingLeaves) {
      await leaf.setViewState({ type: "empty" });
    }
  }
};
var ReplicaSettingTab = class extends import_obsidian2.PluginSettingTab {
  constructor(app, plugin) {
    super(app, plugin);
    this.plugin = plugin;
  }
  display() {
    const { containerEl } = this;
    containerEl.empty();
    new import_obsidian2.Setting(containerEl).setName("Server base URL").setDesc("Replica session endpoint for the Sheaf server.").addText(
      (text) => text.setValue(this.plugin.settings.serverBaseUrl).onChange(async (value) => {
        this.plugin.settings.serverBaseUrl = value.trim() || DEFAULT_SETTINGS.serverBaseUrl;
        await this.plugin.persistSettings();
      })
    );
    new import_obsidian2.Setting(containerEl).setName("Vault name").setDesc("Logical server-side vault name used by replica sync.").addText(
      (text) => text.setValue(this.plugin.settings.vaultName).onChange(async (value) => {
        this.plugin.settings.vaultName = value.trim() || this.app.vault.getName();
        await this.plugin.persistSettings();
      })
    );
    new import_obsidian2.Setting(containerEl).setName("Server root path").setDesc("Required when the server must create the replica vault on first sync.").addText(
      (text) => text.setValue(this.plugin.settings.serverRootPath).onChange(async (value) => {
        this.plugin.settings.serverRootPath = value.trim();
        await this.plugin.persistSettings();
      })
    );
    new import_obsidian2.Setting(containerEl).setName("Create missing vault automatically").addToggle(
      (toggle) => toggle.setValue(this.plugin.settings.createIfMissing).onChange(async (value) => {
        this.plugin.settings.createIfMissing = value;
        await this.plugin.persistSettings();
      })
    );
    new import_obsidian2.Setting(containerEl).setName("Block local edits").setDesc("Reject typing, paste, undo, and redo in replicated notes.").addToggle(
      (toggle) => toggle.setValue(this.plugin.settings.blockLocalEdits).onChange(async (value) => {
        this.plugin.settings.blockLocalEdits = value;
        await this.plugin.persistSettings();
      })
    );
  }
};
var SheafObsidianReplicaPlugin = class extends import_obsidian2.Plugin {
  settings = { ...DEFAULT_SETTINGS };
  stateRepository;
  syncService = null;
  latestState = null;
  repairTimer = null;
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
        const activeFile = this.app.workspace.getActiveViewOfType(import_obsidian2.MarkdownView)?.file;
        if (!(activeFile instanceof import_obsidian2.TFile)) {
          return false;
        }
        return Boolean(this.latestState?.files[(0, import_obsidian2.normalizePath)(activeFile.path)]);
      })
    );
    this.addSettingTab(new ReplicaSettingTab(this.app, this));
    this.addCommand({
      id: "replica-sync-now",
      name: "Sync replica now",
      callback: async () => {
        try {
          await this.syncService?.start();
          new import_obsidian2.Notice("Replica sync started");
        } catch (error) {
          new import_obsidian2.Notice(`Replica sync failed: ${error instanceof Error ? error.message : String(error)}`);
        }
      }
    });
    this.addCommand({
      id: "replica-repair-now",
      name: "Repair replica now",
      callback: async () => {
        try {
          await this.syncService?.repairNow();
          new import_obsidian2.Notice("Replica repair complete");
        } catch (error) {
          new import_obsidian2.Notice(`Replica repair failed: ${error instanceof Error ? error.message : String(error)}`);
        }
      }
    });
    this.addCommand({
      id: "replica-show-status",
      name: "Show replica status",
      callback: async () => {
        const state = this.latestState ?? await this.stateRepository.loadState(this.settings.vaultName);
        const health = state.health;
        new import_obsidian2.Notice(
          `Replica ${health.connectionState}; next LSN ${state.nextLsn}; last good ${health.lastSuccessfulLsn ?? "none"}`,
          6e3
        );
      }
    });
    try {
      await this.syncService.start();
    } catch (error) {
      new import_obsidian2.Notice(`Replica sync failed to start: ${error instanceof Error ? error.message : String(error)}`);
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
    this.syncService?.stop();
    if (this.repairTimer !== null) {
      window.clearInterval(this.repairTimer);
      this.repairTimer = null;
    }
  }
  async persistSettings() {
    await this.stateRepository.saveSettings(this.settings);
  }
};
