import { ChatApiClient } from "./api.js";
import { ChatStore, applyCommittedTurn, dropQueueArtifacts, dropUncommittedArtifacts } from "./store.js";
import { ChatTransportClient } from "./transport.js";
export class ChatService {
    options;
    api;
    transport;
    store = new ChatStore();
    getNow;
    activeThreadID = null;
    connectAttempt = 0;
    watchdogHandle = null;
    reconnectHandle = null;
    thinkingQuietHandle = null;
    constructor(options) {
        this.options = options;
        this.api = new ChatApiClient(options.settings);
        this.transport = new ChatTransportClient(options.settings);
        this.getNow = options.getNow ?? (() => Date.now());
    }
    subscribe(listener) {
        return this.store.subscribe(listener);
    }
    getSnapshot() {
        return this.store.getSnapshot();
    }
    async activateView() {
        await this.showThreadList();
    }
    async deactivateView() {
        this.clearTimers();
        this.transport.disconnect();
        this.activeThreadID = null;
        this.store.showThreadList();
    }
    openSettings() {
        this.options.openSettings();
    }
    async showThreadList() {
        this.clearTimers();
        this.transport.disconnect();
        this.activeThreadID = null;
        this.store.showThreadList();
        await this.refreshThreads();
    }
    async refreshThreads() {
        this.store.setThreadListLoading(true);
        try {
            const threads = await this.api.listThreads();
            this.store.setThreads(threads);
        }
        catch (error) {
            this.store.setThreadListError(error instanceof Error ? error.message : String(error));
        }
    }
    async createThread(name = "New thread") {
        this.store.setThreadListCreating(true);
        try {
            const trimmedName = name.trim() || "New thread";
            const created = await this.api.createThread(trimmedName);
            await this.refreshThreads();
            const thread = this.store.findThread(created.thread_id) ?? {
                thread_id: created.thread_id,
                name: trimmedName,
                prev_thread_id: null,
                start_turn_id: null,
                is_archived: false,
                tail_turn_id: null,
                created_at: null,
                updated_at: null,
            };
            await this.openThread(thread.thread_id, thread);
        }
        catch (error) {
            this.store.setThreadListError(error instanceof Error ? error.message : String(error));
        }
        finally {
            this.store.setThreadListCreating(false);
        }
    }
    async openThread(threadID, providedThread) {
        const thread = providedThread ?? this.store.findThread(threadID);
        if (!thread) {
            throw new Error(`Unknown thread ${threadID}`);
        }
        this.clearTimers();
        this.transport.disconnect();
        const session = this.store.openConversation(thread);
        session.connectionState = "connecting";
        session.errorMessage = null;
        session.statusMessage = "Connecting…";
        this.store.replaceSession(threadID, session);
        this.activeThreadID = threadID;
        const attempt = ++this.connectAttempt;
        try {
            const enter = await this.api.enterThread(threadID, session.lastCommittedTurnID);
            if (attempt !== this.connectAttempt || this.activeThreadID !== threadID) {
                return;
            }
            await this.transport.connect(enter.websocket_url, (event) => {
                this.handleTransportEvent(threadID, event);
            });
            if (attempt !== this.connectAttempt || this.activeThreadID !== threadID) {
                this.transport.disconnect();
                return;
            }
            this.startWatchdog();
        }
        catch (error) {
            this.store.updateSession(threadID, (current) => {
                current.connectionState = "error";
                current.errorMessage = error instanceof Error ? error.message : String(error);
                current.statusMessage = null;
            });
            this.scheduleReconnect(threadID);
        }
    }
    async sendMessage(text) {
        const threadID = this.activeThreadID;
        if (!threadID) {
            return false;
        }
        const trimmed = text.trim();
        if (!trimmed) {
            return false;
        }
        const session = this.store.getSession(threadID);
        if (!session || session.connectionState !== "live") {
            return false;
        }
        const clientMessageID = globalThis.crypto?.randomUUID?.() ?? `${Date.now()}-${Math.random()}`;
        const localMessageID = `local-${clientMessageID}`;
        const modelName = this.options.settings().chatDefaultModel;
        const inResponseToTurnID = session.lastCommittedTurnID ?? null;
        this.store.updateSession(threadID, (session) => {
            session.pendingSends.push({
                clientMessageID,
                text: trimmed,
                responseToTurnID: inResponseToTurnID,
                localMessageID,
                queueID: null,
            });
            session.errorMessage = null;
            session.statusMessage = "Sending…";
        });
        try {
            await this.transport.submitMessage({
                threadID,
                text: trimmed,
                modelName,
                inResponseToTurnID,
                clientMessageID,
            });
        }
        catch (error) {
            this.store.updateSession(threadID, (session) => {
                session.errorMessage = error instanceof Error ? error.message : String(error);
                session.statusMessage = null;
                dropUncommittedArtifacts(session);
            });
            return false;
        }
        return true;
    }
    handleTransportEvent(threadID, event) {
        if (threadID !== this.activeThreadID) {
            return;
        }
        const now = this.getNow();
        if (event.type === "heartbeat") {
            const session = this.store.getSession(threadID);
            if (session) {
                session.lastFrameAtMs = now;
            }
            return;
        }
        if (event.type === "closed") {
            this.store.updateSession(threadID, (session) => {
                session.connectionState = "error";
                session.statusMessage = "Disconnected";
                session.errorMessage = event.reason ?? `Connection closed (${event.code})`;
            });
            this.scheduleReconnect(threadID);
            return;
        }
        this.store.updateSession(threadID, (session) => {
            session.lastFrameAtMs = now;
            switch (event.type) {
                case "handshake_begin":
                    session.connectionState = "replaying";
                    session.errorMessage = null;
                    session.statusMessage = "Loading chat history…";
                    session.committedTurns = [];
                    dropUncommittedArtifacts(session);
                    break;
                case "handshake_ready":
                    session.connectionState = "live";
                    session.errorMessage = null;
                    session.statusMessage = null;
                    break;
                case "durable_ack":
                    if (event.clientMessageID) {
                        const pending = session.pendingSends.find((item) => item.clientMessageID === event.clientMessageID);
                        if (pending) {
                            pending.queueID = event.queueID;
                        }
                    }
                    session.statusMessage = "Waiting for response…";
                    break;
                case "assistant_token":
                    session.streamingByQueue[event.queueID] = {
                        queueID: event.queueID,
                        text: `${session.streamingByQueue[event.queueID]?.text ?? ""}${event.chunk}`,
                    };
                    session.statusMessage = "Assistant is responding…";
                    break;
                case "committed_turn":
                    applyCommittedTurn(session, event.turn);
                    session.statusMessage = null;
                    break;
                case "turn_finalized":
                    delete session.streamingByQueue[event.queueID];
                    session.statusMessage = null;
                    session.thinkingActive = false;
                    break;
                case "execution_conflict":
                    session.connectionState = "connecting";
                    session.errorMessage = "Chat changed elsewhere. Resyncing…";
                    session.statusMessage = "Resyncing…";
                    dropUncommittedArtifacts(session);
                    this.transport.disconnect();
                    this.stopWatchdog();
                    this.scheduleReconnect(threadID);
                    return;
                case "context_budget":
                    session.contextBudget = event.context;
                    break;
                case "turn_event":
                    if (event.event === "execution_started") {
                        session.thinkingActive = true;
                        session.statusMessage = "Thinking…";
                    }
                    else if (event.event === "thinking_trace") {
                        session.thinkingActive = true;
                        session.statusMessage = "Thinking…";
                    }
                    else if (event.event === "context_compaction") {
                        session.statusMessage = "Condensing context…";
                    }
                    this.bumpThinkingQuietTimer(threadID);
                    break;
                case "error":
                    session.errorMessage = event.message;
                    session.statusMessage = null;
                    if (event.queueID !== null && event.queueID >= 0) {
                        if (event.fatal) {
                            dropQueueArtifacts(session, event.queueID);
                        }
                        else {
                            delete session.streamingByQueue[event.queueID];
                            if (Object.keys(session.streamingByQueue).length === 0) {
                                session.thinkingActive = false;
                            }
                        }
                        session.statusMessage = this.deriveInFlightStatus(session);
                        break;
                    }
                    session.connectionState = "error";
                    session.thinkingActive = false;
                    if (event.fatal) {
                        dropUncommittedArtifacts(session);
                    }
                    break;
            }
        });
    }
    deriveInFlightStatus(session) {
        if (Object.keys(session.streamingByQueue).length > 0) {
            return "Assistant is responding…";
        }
        if (session.pendingSends.some((pending) => pending.queueID !== null)) {
            return "Waiting for response…";
        }
        if (session.pendingSends.length > 0) {
            return "Sending…";
        }
        return null;
    }
    scheduleReconnect(threadID) {
        if (this.reconnectHandle !== null || threadID !== this.activeThreadID) {
            return;
        }
        this.reconnectHandle = window.setTimeout(() => {
            this.reconnectHandle = null;
            const thread = this.store.findThread(threadID);
            if (!thread || this.activeThreadID !== threadID) {
                return;
            }
            void this.openThread(threadID, thread);
        }, this.options.settings().chatReconnectDelayMs);
    }
    startWatchdog() {
        this.stopWatchdog();
        this.watchdogHandle = window.setInterval(() => {
            const threadID = this.activeThreadID;
            const session = threadID ? this.store.getSession(threadID) : null;
            if (!threadID || !session || session.connectionState === "connecting") {
                return;
            }
            const lastFrameAtMs = session.lastFrameAtMs ?? 0;
            if (lastFrameAtMs === 0) {
                return;
            }
            if (this.getNow() - lastFrameAtMs < this.options.settings().chatWatchdogMs) {
                return;
            }
            this.store.updateSession(threadID, (current) => {
                current.connectionState = "connecting";
                current.errorMessage = "Connection stalled. Reconnecting…";
                current.statusMessage = "Reconnecting…";
                dropUncommittedArtifacts(current);
            });
            void this.openThread(threadID, session.thread);
        }, 5_000);
    }
    stopWatchdog() {
        if (this.watchdogHandle !== null) {
            window.clearInterval(this.watchdogHandle);
            this.watchdogHandle = null;
        }
    }
    bumpThinkingQuietTimer(threadID) {
        if (this.thinkingQuietHandle !== null) {
            window.clearTimeout(this.thinkingQuietHandle);
        }
        this.thinkingQuietHandle = window.setTimeout(() => {
            this.thinkingQuietHandle = null;
            const session = this.store.getSession(threadID);
            if (!session) {
                return;
            }
            this.store.updateSession(threadID, (current) => {
                if (Object.keys(current.streamingByQueue).length === 0) {
                    current.thinkingActive = false;
                    if (current.statusMessage === "Thinking…") {
                        current.statusMessage = null;
                    }
                }
            });
        }, 2_500);
    }
    clearTimers() {
        this.stopWatchdog();
        if (this.reconnectHandle !== null) {
            window.clearTimeout(this.reconnectHandle);
            this.reconnectHandle = null;
        }
        if (this.thinkingQuietHandle !== null) {
            window.clearTimeout(this.thinkingQuietHandle);
            this.thinkingQuietHandle = null;
        }
    }
}
