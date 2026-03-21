import test from "node:test";
import assert from "node:assert/strict";
import { ReplicaReplayEngine } from "../src/replay.js";
function baseState() {
    return {
        version: 1,
        vaultName: "test",
        nextLsn: 2,
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
function checksum(text) {
    const crypto = globalThis.crypto;
    if (!crypto?.subtle) {
        throw new Error("Missing crypto.subtle");
    }
    return "";
}
async function sha(text) {
    const digest = await globalThis.crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
    return Array.from(new Uint8Array(digest))
        .map((value) => value.toString(16).padStart(2, "0"))
        .join("");
}
class MemoryAdapter {
    files = new Map();
    writes = [];
    async read(path) {
        return this.files.get(path)?.content ?? null;
    }
    async write(path, content) {
        this.writes.push({ path, content });
        this.files.set(path, { content, mtime: Date.now() });
    }
    async delete(path) {
        this.files.delete(path);
    }
    async stat(path) {
        const entry = this.files.get(path);
        return entry ? { mtime: entry.mtime } : null;
    }
    async listFiles() {
        return [...this.files.keys()];
    }
}
test("patch replay applies unified diff before falling back", async () => {
    const adapter = new MemoryAdapter();
    adapter.files.set("note.md", { content: "one\n", mtime: 1 });
    let fetchCalls = 0;
    const remote = {
        async fetchRawFile() {
            fetchCalls += 1;
            return { path: "note.md", exists: true, deleted: false, checksum: await sha("two\n"), content: "two\n", last_lsn: 2 };
        },
        async queryPathState() {
            throw new Error("not used");
        },
    };
    const engine = new ReplicaReplayEngine(adapter, remote);
    const record = {
        lsn: 2,
        path: "note.md",
        action: "patch",
        checksum: await sha("two\n"),
        payload: {
            patch: "--- note.md\n+++ note.md\n@@ -1,1 +1,1 @@\n-one\n+two",
        },
    };
    const state = await engine.applyRecord(baseState(), record);
    assert.equal(fetchCalls, 0);
    assert.equal(await adapter.read("note.md"), "two\n");
    assert.equal(state.nextLsn, 3);
});
test("patch replay falls back to raw file when patch application fails", async () => {
    const adapter = new MemoryAdapter();
    adapter.files.set("note.md", { content: "different\n", mtime: 1 });
    let fetchCalls = 0;
    const remote = {
        async fetchRawFile() {
            fetchCalls += 1;
            return {
                path: "note.md",
                exists: true,
                deleted: false,
                checksum: await sha("two\n"),
                content: "two\n",
                last_lsn: 2,
            };
        },
        async queryPathState() {
            throw new Error("not used");
        },
    };
    const engine = new ReplicaReplayEngine(adapter, remote);
    const record = {
        lsn: 2,
        path: "note.md",
        action: "patch",
        checksum: await sha("two\n"),
        payload: {
            patch: "--- note.md\n+++ note.md\n@@ -1,1 +1,1 @@\n-one\n+two",
        },
    };
    const state = await engine.applyRecord(baseState(), record);
    assert.equal(fetchCalls, 1);
    assert.equal(await adapter.read("note.md"), "two\n");
    assert.equal(state.nextLsn, 3);
});
