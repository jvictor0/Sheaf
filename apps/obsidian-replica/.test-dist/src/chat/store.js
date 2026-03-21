import { summarizeToolCall } from "./toolSummary.js";
function buildTranscriptItems(session) {
    const items = [];
    for (const turn of session.committedTurns) {
        if (turn.speaker === "assistant") {
            for (const call of turn.tool_calls) {
                items.push({
                    kind: "tool_call",
                    id: `tool-${turn.id}-${call.id}`,
                    text: summarizeToolCall(call),
                    tone: call.isError ? "error" : "normal",
                });
            }
        }
        items.push({
            kind: "committed",
            id: turn.id,
            role: turn.speaker === "assistant" || turn.speaker === "system" ? turn.speaker : "user",
            text: turn.message_text,
        });
    }
    for (const pending of session.pendingSends) {
        items.push({
            kind: "pending",
            id: pending.localMessageID,
            role: "user",
            text: pending.text,
        });
    }
    for (const stream of Object.values(session.streamingByQueue).sort((a, b) => a.queueID - b.queueID)) {
        items.push({
            kind: "streaming",
            id: `stream-${stream.queueID}`,
            role: "assistant",
            text: stream.text,
            queueID: stream.queueID,
        });
    }
    return items;
}
export function createChatSession(thread) {
    return {
        thread,
        committedTurns: [],
        pendingSends: [],
        streamingByQueue: {},
        lastCommittedTurnID: null,
        lastFrameAtMs: null,
        errorMessage: null,
        connectionState: "idle",
        thinkingActive: false,
        statusMessage: null,
        contextBudget: null,
        transcriptItems: [],
    };
}
export function consumeMatchingPendingSend(session, turn) {
    if (turn.speaker !== "user") {
        return;
    }
    const index = session.pendingSends.findIndex((pending) => {
        if (pending.text !== turn.message_text) {
            return false;
        }
        return pending.responseToTurnID === turn.prev_turn_id || pending.responseToTurnID === session.lastCommittedTurnID;
    });
    if (index >= 0) {
        session.pendingSends.splice(index, 1);
    }
}
export function rebuildTranscript(session) {
    session.transcriptItems = buildTranscriptItems(session);
    return session;
}
export function dropUncommittedArtifacts(session) {
    session.pendingSends = [];
    session.streamingByQueue = {};
    session.thinkingActive = false;
    session.statusMessage = null;
    return rebuildTranscript(session);
}
export function dropQueueArtifacts(session, queueID) {
    session.pendingSends = session.pendingSends.filter((pending) => pending.queueID !== queueID);
    delete session.streamingByQueue[queueID];
    if (Object.keys(session.streamingByQueue).length === 0) {
        session.thinkingActive = false;
    }
    if (Object.keys(session.streamingByQueue).length === 0 && session.pendingSends.length === 0) {
        session.statusMessage = null;
    }
    return rebuildTranscript(session);
}
export function applyCommittedTurn(session, turn) {
    if (session.committedTurns.some((existing) => existing.id === turn.id)) {
        return session;
    }
    session.committedTurns.push(turn);
    session.lastCommittedTurnID = turn.id;
    consumeMatchingPendingSend(session, turn);
    return rebuildTranscript(session);
}
export class ChatStore {
    listeners = new Set();
    sessions = new Map();
    state = {
        screen: "threads",
        threadList: {
            loading: false,
            creating: false,
            errorMessage: null,
            threads: [],
        },
        activeThreadId: null,
        activeSession: null,
    };
    subscribe(listener) {
        this.listeners.add(listener);
        return () => {
            this.listeners.delete(listener);
        };
    }
    getSnapshot() {
        return this.state;
    }
    getActiveSession() {
        return this.state.activeSession;
    }
    getSession(threadID) {
        return this.sessions.get(threadID) ?? null;
    }
    setThreadListLoading(loading) {
        this.state = {
            ...this.state,
            threadList: {
                ...this.state.threadList,
                loading,
                errorMessage: loading ? null : this.state.threadList.errorMessage,
            },
        };
        this.emit();
    }
    setThreadListCreating(creating) {
        this.state = {
            ...this.state,
            threadList: {
                ...this.state.threadList,
                creating,
            },
        };
        this.emit();
    }
    setThreadListError(message) {
        this.state = {
            ...this.state,
            threadList: {
                ...this.state.threadList,
                loading: false,
                errorMessage: message,
            },
        };
        this.emit();
    }
    setThreads(threads) {
        const existingSessions = new Map();
        for (const thread of threads) {
            const current = this.sessions.get(thread.thread_id);
            existingSessions.set(thread.thread_id, rebuildTranscript({ ...(current ?? createChatSession(thread)), thread }));
        }
        for (const [threadID, session] of this.sessions.entries()) {
            if (!existingSessions.has(threadID)) {
                existingSessions.set(threadID, session);
            }
        }
        this.sessions.clear();
        for (const [threadID, session] of existingSessions.entries()) {
            this.sessions.set(threadID, session);
        }
        this.state = {
            ...this.state,
            threadList: {
                ...this.state.threadList,
                loading: false,
                errorMessage: null,
                threads,
            },
            activeSession: this.state.activeThreadId ? this.sessions.get(this.state.activeThreadId) ?? null : null,
        };
        this.emit();
    }
    showThreadList() {
        this.state = {
            ...this.state,
            screen: "threads",
            activeThreadId: null,
            activeSession: null,
        };
        this.emit();
    }
    openConversation(thread) {
        const current = this.sessions.get(thread.thread_id);
        const session = rebuildTranscript({ ...(current ?? createChatSession(thread)), thread });
        this.sessions.set(thread.thread_id, session);
        this.state = {
            ...this.state,
            screen: "conversation",
            activeThreadId: thread.thread_id,
            activeSession: session,
        };
        this.emit();
        return session;
    }
    updateSession(threadID, updater) {
        const session = this.sessions.get(threadID);
        if (!session) {
            throw new Error(`Unknown chat session for thread ${threadID}`);
        }
        updater(session);
        rebuildTranscript(session);
        if (this.state.activeThreadId === threadID) {
            this.state = {
                ...this.state,
                activeSession: session,
            };
        }
        this.emit();
        return session;
    }
    replaceSession(threadID, session) {
        this.sessions.set(threadID, rebuildTranscript(session));
        if (this.state.activeThreadId === threadID) {
            this.state = {
                ...this.state,
                activeSession: this.sessions.get(threadID) ?? null,
            };
        }
        this.emit();
    }
    findThread(threadID) {
        const inList = this.state.threadList.threads.find((thread) => thread.thread_id === threadID);
        if (inList) {
            return inList;
        }
        return this.sessions.get(threadID)?.thread ?? null;
    }
    emit() {
        for (const listener of this.listeners) {
            listener();
        }
    }
}
