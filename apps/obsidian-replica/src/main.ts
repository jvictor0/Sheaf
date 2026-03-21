import {
  App,
  ItemView,
  MarkdownView,
  Notice,
  Plugin,
  PluginSettingTab,
  Setting,
  TFile,
  WorkspaceLeaf,
  requestUrl,
  normalizePath,
} from "obsidian";

import { ChatService } from "./chat/service";
import { SHEAF_CHAT_VIEW_TYPE, SheafChatView } from "./chat/view";
import { createReplicaEditBlocker } from "./editProtection";
import { ReplicaRemoteReader, ReplicaReplayEngine, ReplicaVaultAdapter } from "./replay";
import { ReplicaStateRepository } from "./state";
import { ChatModelOption, DEFAULT_SETTINGS, ReplicaPluginSettings, ReplicaVaultState } from "./types";
import { ReplicaSyncService } from "./syncClient";

class ObsidianVaultAdapter implements ReplicaVaultAdapter {
  constructor(private readonly app: App) {}

  async read(path: string): Promise<string | null> {
    const file = this.app.vault.getAbstractFileByPath(normalizePath(path));
    if (!(file instanceof TFile)) {
      return null;
    }
    return this.app.vault.cachedRead(file);
  }

  async write(path: string, content: string): Promise<void> {
    const normalized = normalizePath(path);
    const existing = this.app.vault.getAbstractFileByPath(normalized);
    if (existing instanceof TFile) {
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

  async delete(path: string): Promise<void> {
    const file = this.app.vault.getAbstractFileByPath(normalizePath(path));
    if (file instanceof TFile) {
      await this.closeOpenLeaves(file);
      await this.app.vault.delete(file, true);
      this.app.workspace.trigger("layout-change");
      return;
    }
    if (file) {
      await this.app.vault.delete(file, true);
    }
  }

  async stat(path: string): Promise<{ mtime: number } | null> {
    const stat = await this.app.vault.adapter.stat(normalizePath(path));
    if (!stat) {
      return null;
    }
    return { mtime: stat.mtime };
  }

  async listFiles(): Promise<string[]> {
    return this.app.vault.getFiles().map((file) => file.path);
  }

  private async ensureFolders(path: string): Promise<void> {
    const parts = normalizePath(path).split("/").slice(0, -1);
    let current = "";
    for (const part of parts) {
      current = current ? `${current}/${part}` : part;
      if (this.app.vault.getAbstractFileByPath(current)) {
        continue;
      }
      await this.app.vault.createFolder(current);
    }
  }

  private async refreshOpenLeaves(file: TFile, content: string): Promise<void> {
    const matchingLeaves = this.app.workspace
      .getLeavesOfType("markdown")
      .filter((leaf) => leaf.view instanceof MarkdownView && leaf.view.file?.path === file.path);

    for (const leaf of matchingLeaves) {
      const view = leaf.view;
      if (!(view instanceof MarkdownView)) {
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
      new Notice(`Replica refreshed ${file.path}`, 2000);
      this.app.workspace.trigger("layout-change");
    }
  }

  private async closeOpenLeaves(file: TFile): Promise<void> {
    const matchingLeaves = this.app.workspace
      .getLeavesOfType("markdown")
      .filter((leaf) => leaf.view instanceof MarkdownView && leaf.view.file?.path === file.path);

    for (const leaf of matchingLeaves) {
      await leaf.setViewState({ type: "empty" });
    }
  }
}

class ReplicaSettingTab extends PluginSettingTab {
  constructor(
    app: App,
    private readonly plugin: SheafObsidianReplicaPlugin,
  ) {
    super(app, plugin);
  }

  display(): void {
    const { containerEl } = this;
    containerEl.empty();

    new Setting(containerEl)
      .setName("Server base URL")
      .setDesc("Replica session endpoint for the Sheaf server.")
      .addText((text) =>
        text.setValue(this.plugin.settings.serverBaseUrl).onChange(async (value) => {
          this.plugin.settings.serverBaseUrl = value.trim() || DEFAULT_SETTINGS.serverBaseUrl;
          await this.plugin.persistSettings();
        }),
      );

    new Setting(containerEl)
      .setName("Vault name")
      .setDesc("Logical server-side vault name used by replica sync.")
      .addText((text) =>
        text.setValue(this.plugin.settings.vaultName).onChange(async (value) => {
          this.plugin.settings.vaultName = value.trim() || this.app.vault.getName();
          await this.plugin.persistSettings();
        }),
      );

    new Setting(containerEl)
      .setName("Server root path")
      .setDesc("Required when the server must create the replica vault on first sync.")
      .addText((text) =>
        text.setValue(this.plugin.settings.serverRootPath).onChange(async (value) => {
          this.plugin.settings.serverRootPath = value.trim();
          await this.plugin.persistSettings();
        }),
      );

    new Setting(containerEl)
      .setName("Create missing vault automatically")
      .addToggle((toggle) =>
        toggle.setValue(this.plugin.settings.createIfMissing).onChange(async (value) => {
          this.plugin.settings.createIfMissing = value;
          await this.plugin.persistSettings();
        }),
      );

    new Setting(containerEl)
      .setName("Block local edits")
      .setDesc("Reject typing, paste, undo, and redo in replicated notes.")
      .addToggle((toggle) =>
        toggle.setValue(this.plugin.settings.blockLocalEdits).onChange(async (value) => {
          this.plugin.settings.blockLocalEdits = value;
          await this.plugin.persistSettings();
        }),
      );

    new Setting(containerEl)
      .setName("Chat default model")
      .setDesc("Choose which model new chat messages should use.")
      .addDropdown((dropdown) => {
        dropdown.addOption("", "Server default");
        const models = this.plugin.availableChatModels;
        for (const model of models) {
          const suffix = model.is_default ? " (default)" : "";
          dropdown.addOption(model.name, `${model.name} · ${model.provider}${suffix}`);
        }
        if (this.plugin.settings.chatDefaultModel && !models.some((model) => model.name === this.plugin.settings.chatDefaultModel)) {
          dropdown.addOption(this.plugin.settings.chatDefaultModel, `${this.plugin.settings.chatDefaultModel} · custom`);
        }
        dropdown.setValue(this.plugin.settings.chatDefaultModel).onChange(async (value) => {
          this.plugin.settings.chatDefaultModel = value.trim();
          await this.plugin.persistSettings();
        });
      })
      .addButton((button) => {
        button.setButtonText("Refresh").onClick(async () => {
          await this.plugin.refreshAvailableChatModels(true);
          this.display();
        });
      });

    new Setting(containerEl)
      .setName("Chat reconnect delay (ms)")
      .setDesc("Delay before retrying after a chat disconnect or conflict.")
      .addText((text) =>
        text.setValue(String(this.plugin.settings.chatReconnectDelayMs)).onChange(async (value) => {
          const parsed = Number.parseInt(value, 10);
          this.plugin.settings.chatReconnectDelayMs = Number.isFinite(parsed)
            ? Math.max(parsed, 250)
            : DEFAULT_SETTINGS.chatReconnectDelayMs;
          await this.plugin.persistSettings();
        }),
      );

    new Setting(containerEl)
      .setName("Chat watchdog timeout (ms)")
      .setDesc("Reconnect the chat pane if no websocket frames arrive within this window.")
      .addText((text) =>
        text.setValue(String(this.plugin.settings.chatWatchdogMs)).onChange(async (value) => {
          const parsed = Number.parseInt(value, 10);
          this.plugin.settings.chatWatchdogMs = Number.isFinite(parsed)
            ? Math.max(parsed, 5_000)
            : DEFAULT_SETTINGS.chatWatchdogMs;
          await this.plugin.persistSettings();
        }),
      );
  }
}

export default class SheafObsidianReplicaPlugin extends Plugin {
  settings: ReplicaPluginSettings = { ...DEFAULT_SETTINGS };
  availableChatModels: ChatModelOption[] = [];
  private stateRepository!: ReplicaStateRepository;
  private syncService: ReplicaSyncService | null = null;
  private chatService!: ChatService;
  private latestState: ReplicaVaultState | null = null;
  private repairTimer: number | null = null;
  private settingTab!: ReplicaSettingTab;

  async onload(): Promise<void> {
    this.stateRepository = new ReplicaStateRepository(this);
    this.settings = await this.stateRepository.loadSettings(this.app.vault.getName());
    this.latestState = await this.stateRepository.loadState(this.settings.vaultName);

    const adapter = new ObsidianVaultAdapter(this.app);
    const remoteReader: ReplicaRemoteReader = {
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
      },
    };

    const replayEngine = new ReplicaReplayEngine(adapter, remoteReader);
    this.syncService = new ReplicaSyncService(
      this.stateRepository,
      () => this.settings,
      replayEngine,
      async (state) => {
        this.latestState = state;
      },
    );

    this.registerEditorExtension(
      createReplicaEditBlocker(() => {
        if (!this.settings.blockLocalEdits) {
          return false;
        }
        const activeFile = this.app.workspace.getActiveViewOfType(MarkdownView)?.file;
        if (!(activeFile instanceof TFile)) {
          return false;
        }
        return Boolean(this.latestState?.files[normalizePath(activeFile.path)]);
      }),
    );
    this.chatService = new ChatService({
      settings: () => this.settings,
      openSettings: () => this.openPluginSettings(),
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
          new Notice("Replica sync started");
        } catch (error) {
          new Notice(`Replica sync failed: ${error instanceof Error ? error.message : String(error)}`);
        }
      },
    });

    this.addCommand({
      id: "replica-repair-now",
      name: "Repair replica now",
      callback: async () => {
        try {
          await this.syncService?.repairNow();
          new Notice("Replica repair complete");
        } catch (error) {
          new Notice(`Replica repair failed: ${error instanceof Error ? error.message : String(error)}`);
        }
      },
    });

    this.addCommand({
      id: "replica-show-status",
      name: "Show replica status",
      callback: async () => {
        const state = this.latestState ?? (await this.stateRepository.loadState(this.settings.vaultName));
        const health = state.health;
        new Notice(
          `Replica ${health.connectionState}; next LSN ${state.nextLsn}; last good ${health.lastSuccessfulLsn ?? "none"}`,
          6000,
        );
      },
    });

    this.addCommand({
      id: "open-chat-pane",
      name: "Open Sheaf chat",
      callback: async () => {
        await this.activateChatView();
      },
    });

    try {
      await this.syncService.start();
    } catch (error) {
      new Notice(`Replica sync failed to start: ${error instanceof Error ? error.message : String(error)}`);
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

  onunload(): void {
    void this.chatService?.deactivateView();
    this.app.workspace.getLeavesOfType(SHEAF_CHAT_VIEW_TYPE).forEach((leaf) => leaf.detach());
    this.syncService?.stop();
    if (this.repairTimer !== null) {
      window.clearInterval(this.repairTimer);
      this.repairTimer = null;
    }
  }

  async persistSettings(): Promise<void> {
    await this.stateRepository.saveSettings(this.settings);
  }

  async refreshAvailableChatModels(showFailureNotice: boolean): Promise<void> {
    try {
      const response = await requestUrl({
        url: `${this.settings.serverBaseUrl}/models`,
        method: "GET",
      });
      const payload = response.json as { models?: Array<Record<string, unknown>> };
      const models = Array.isArray(payload.models) ? payload.models : [];
      this.availableChatModels = models
        .filter((item): item is Record<string, unknown> => typeof item?.name === "string")
        .map((item) => ({
          name: String(item.name),
          provider: typeof item.provider === "string" ? item.provider : "unknown",
          is_default: item.is_default === true,
        }));
      this.settingTab?.display();
    } catch (error) {
      if (showFailureNotice) {
        new Notice(`Failed to load models: ${error instanceof Error ? error.message : String(error)}`);
      }
    }
  }

  private async activateChatView(): Promise<void> {
    const existing = this.app.workspace.getLeavesOfType(SHEAF_CHAT_VIEW_TYPE)[0];
    const leaf = existing ?? this.app.workspace.getLeaf(true);
    await leaf.setViewState({ type: SHEAF_CHAT_VIEW_TYPE, active: true });
    this.app.workspace.revealLeaf(leaf);
  }

  private openPluginSettings(): void {
    const settingAPI = (this.app as App & {
      setting?: {
        open: () => void;
        openTabById?: (id: string) => void;
      };
    }).setting;

    settingAPI?.open?.();
    settingAPI?.openTabById?.(this.manifest.id);
  }
}
