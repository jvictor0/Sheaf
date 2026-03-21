import test from "node:test";
import assert from "node:assert/strict";
import { ChatService } from "../src/chat/service.js";
const thread = {
    thread_id: "thread-1",
    name: "Test thread",
    prev_thread_id: null,
    start_turn_id: null,
    is_archived: false,
    tail_turn_id: null,
    created_at: null,
    updated_at: null,
};
function createService() {
    return new ChatService({
        settings: () => ({
            serverBaseUrl: "http://127.0.0.1:2731",
            vaultName: "test",
            createIfMissing: true,
            serverRootPath: "",
            blockLocalEdits: true,
            repairIntervalMs: 60_000,
            reconnectDelayMs: 2_000,
            chatDefaultModel: "test-model",
            chatWatchdogMs: 45_000,
            chatReconnectDelayMs: 25,
        }),
        openSettings: () => { },
        getNow: () => 1000,
    });
}
test("fatal error frames clear pending and streaming artifacts", () => {
    const previousWindow = globalThis.window;
    globalThis.window = globalThis;
    try {
        const service = createService();
        const store = service.store;
        const session = store.openConversation(thread);
        session.pendingSends.push({
            clientMessageID: "client-1",
            text: "hello",
            responseToTurnID: null,
            localMessageID: "local-1",
            queueID: 7,
        });
        session.pendingSends.push({
            clientMessageID: "client-2",
            text: "other",
            responseToTurnID: null,
            localMessageID: "local-2",
            queueID: 8,
        });
        session.streamingByQueue[7] = { queueID: 7, text: "streaming" };
        session.streamingByQueue[8] = { queueID: 8, text: "other streaming" };
        session.thinkingActive = true;
        session.statusMessage = "Assistant is responding…";
        session.connectionState = "live";
        service.activeThreadID = thread.thread_id;
        service.handleTransportEvent(thread.thread_id, {
            type: "error",
            message: "Queue failed",
            queueID: 7,
            fatal: true,
        });
        assert.deepEqual(session.pendingSends.map((pending) => pending.clientMessageID), ["client-2"]);
        assert.deepEqual(Object.keys(session.streamingByQueue), ["8"]);
        assert.equal(session.thinkingActive, true);
        assert.equal(session.statusMessage, "Assistant is responding…");
        assert.equal(session.errorMessage, "Queue failed");
        assert.equal(session.connectionState, "live");
    }
    finally {
        if (previousWindow) {
            globalThis.window = previousWindow;
        }
        else {
            delete globalThis.window;
        }
    }
});
test("execution conflict reconnect is cancelled when leaving the thread", async () => {
    const previousWindow = globalThis.window;
    globalThis.window = globalThis;
    try {
        const service = createService();
        const store = service.store;
        store.openConversation(thread);
        service.activeThreadID = thread.thread_id;
        let reopenCount = 0;
        service.openThread =
            async () => {
                reopenCount += 1;
            };
        service.handleTransportEvent(thread.thread_id, {
            type: "execution_conflict",
            queueID: 11,
            expectedTailTurnID: "turn-a",
            actualTailTurnID: "turn-b",
        });
        await service.deactivateView();
        await new Promise((resolve) => setTimeout(resolve, 60));
        assert.equal(reopenCount, 0);
    }
    finally {
        if (previousWindow) {
            globalThis.window = previousWindow;
        }
        else {
            delete globalThis.window;
        }
    }
});
test("sendMessage is blocked until replay is ready", async () => {
    const service = createService();
    const store = service.store;
    const session = store.openConversation(thread);
    session.connectionState = "replaying";
    service.activeThreadID = thread.thread_id;
    const sentWhileReplaying = await service.sendMessage("Hello before replay");
    assert.equal(sentWhileReplaying, false);
    assert.equal(store.getSession(thread.thread_id)?.pendingSends.length, 0);
    session.connectionState = "live";
    let submittedText = null;
    service.transport.submitMessage = async (args) => {
        submittedText = args.text;
    };
    const sentWhenLive = await service.sendMessage("Hello after replay");
    assert.equal(sentWhenLive, true);
    assert.equal(submittedText, "Hello after replay");
    assert.equal(store.getSession(thread.thread_id)?.pendingSends.length, 1);
});
