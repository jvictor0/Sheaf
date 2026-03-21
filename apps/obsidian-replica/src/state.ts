import { Plugin } from "obsidian";

import {
  DEFAULT_SETTINGS,
  ReplicaPluginData,
  ReplicaPluginSettings,
  ReplicaVaultState,
  createDefaultVaultState,
} from "./types";

function normalizeState(input: Partial<ReplicaVaultState> | undefined, vaultName: string): ReplicaVaultState {
  const base = createDefaultVaultState(vaultName);
  return {
    version: 1,
    vaultName: input?.vaultName || vaultName,
    nextLsn: typeof input?.nextLsn === "number" ? input.nextLsn : base.nextLsn,
    files: input?.files ?? base.files,
    health: {
      ...base.health,
      ...(input?.health ?? {}),
    },
  };
}

export class ReplicaStateRepository {
  constructor(private readonly plugin: Plugin) {}

  private async loadPluginData(): Promise<ReplicaPluginData> {
    const loaded = (await this.plugin.loadData()) as ReplicaPluginData | null;
    return loaded ?? {};
  }

  async loadSettings(defaultVaultName: string): Promise<ReplicaPluginSettings> {
    const pluginData = await this.loadPluginData();
    return {
      ...DEFAULT_SETTINGS,
      vaultName: defaultVaultName,
      ...(pluginData.settings ?? {}),
    };
  }

  async saveSettings(settings: ReplicaPluginSettings): Promise<void> {
    const pluginData = await this.loadPluginData();
    await this.plugin.saveData({
      ...pluginData,
      settings,
    } satisfies ReplicaPluginData);
  }

  async loadState(vaultName: string): Promise<ReplicaVaultState> {
    const pluginData = await this.loadPluginData();
    return normalizeState(pluginData.state, vaultName);
  }

  async saveState(state: ReplicaVaultState): Promise<void> {
    const pluginData = await this.loadPluginData();
    await this.plugin.saveData({
      ...pluginData,
      state,
    } satisfies ReplicaPluginData);
  }
}
