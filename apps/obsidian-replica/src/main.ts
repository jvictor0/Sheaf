import {
  App,
  MarkdownView,
  Notice,
  Plugin,
  PluginSettingTab,
  Setting,
  TFile,
  normalizePath,
} from "obsidian";

import { createReplicaEditBlocker } from "./editProtection";
import { ReplicaRemoteReader, ReplicaReplayEngine, ReplicaVaultAdapter } from "./replay";
import { ReplicaStateRepository } from "./state";
import { DEFAULT_SETTINGS, ReplicaPluginSettings, ReplicaVaultState } from "./types";
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
  }
}

export default class SheafObsidianReplicaPlugin extends Plugin {
  settings: ReplicaPluginSettings = { ...DEFAULT_SETTINGS };
  private stateRepository!: ReplicaStateRepository;
  private syncService: ReplicaSyncService | null = null;
  private latestState: ReplicaVaultState | null = null;
  private repairTimer: number | null = null;

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
    this.addSettingTab(new ReplicaSettingTab(this.app, this));

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
    this.syncService?.stop();
    if (this.repairTimer !== null) {
      window.clearInterval(this.repairTimer);
      this.repairTimer = null;
    }
  }

  async persistSettings(): Promise<void> {
    await this.stateRepository.saveSettings(this.settings);
  }
}
