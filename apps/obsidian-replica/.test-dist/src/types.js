export const DEFAULT_SETTINGS = {
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
export function createDefaultVaultState(vaultName) {
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
