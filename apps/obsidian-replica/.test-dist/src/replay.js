import { sha256Text } from "./checksum.js";
import { normalizeReplicaPath } from "./path.js";
import { createDefaultVaultState } from "./types.js";
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
            }
            else if (marker === "-") {
                if ((originalLines[originalIndex] ?? "") !== value) {
                    throw new Error(`Patch deletion mismatch at line ${originalIndex + 1}`);
                }
                originalIndex += 1;
            }
            else if (marker === "+") {
                result.push(value);
            }
            else if (line.length === 0) {
                result.push("");
            }
            else {
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
            unhealthyPath: null,
        },
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
            unhealthyPath: null,
        },
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
            resyncRequired: true,
        },
    };
}
export class ReplicaReplayEngine {
    adapter;
    remote;
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
                            syncedMtimeMs: stat?.mtime ?? Date.now(),
                        },
                    },
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
                }
                catch {
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
                            syncedMtimeMs: stat.mtime,
                        },
                    },
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
            files,
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
                    syncedMtimeMs: stat.mtime,
                },
            },
        };
    }
    async recoverPath(state, lsn, path) {
        const remoteState = await this.remote.fetchRawFile(path);
        if (!remoteState.exists || remoteState.deleted || remoteState.content === null || remoteState.checksum === null) {
            return this.applyDelete(state, lsn, path);
        }
        try {
            return await this.writeVerifiedContent(state, lsn, path, remoteState.content, remoteState.checksum);
        }
        catch (error) {
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
                files,
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
                        syncedMtimeMs: stat.mtime,
                    },
                },
            };
        }
        catch (error) {
            return setUnhealthy(state, path, error instanceof Error ? error.message : String(error));
        }
    }
}
